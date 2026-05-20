/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

/**
 * Raw GPU-kernel bandwidth benchmark for pinned host memory (and HBM
 * baseline). Designed to answer:
 *   - What is the upper bound on bandwidth when a GPU kernel writes
 *     directly to pinned host memory? (= PCIe-bounded.)
 *   - What does the same kernel achieve writing to HBM? (= HBM-bounded.)
 *   - Does interleaving with concurrent flush-style activity slow it
 *     down? (= contention check.)
 *
 * Layout mirrors gpu_vector's storage: each "block" gets a separate
 * buffer in either DRAM (pinned host) or HBM (device). Each thread
 * block in the grid maps to one logical buffer block; 32 lanes per
 * grid block warp-cooperate on a 1 MiB page-sized region inside the
 * buffer, looping until the entire buffer is touched.
 *
 * Knobs:
 *   --blocks N             How many concurrent buffer blocks (default 4)
 *   --per-block-bytes B    Bytes per block (default 16 MiB)
 *   --page-bytes P         Page granularity for the inner stride loop
 *                          (default 1 MiB; mirrors gpu_vector page size)
 *   --target {pinned,hbm,both}  Buffer kind (default both)
 *   --iters N              Repeats; first iter is warmup (default 6)
 *   --threads-per-block T  Threads in each grid block (default 32)
 *   --concurrent-flush     Run a second kernel concurrently on a
 *                          non-blocking stream writing the same volume
 *                          (mimics manager Phase 3 flush contention).
 *   --read                 Run read kernels instead of write
 *
 * Reports min / median / max wall time + median bandwidth per iter.
 */

#if (CTP_ENABLE_CUDA || CTP_ENABLE_ROCM) && !CTP_ENABLE_SYCL

#include <clio_ctp/util/gpu_api.h>

#if CTP_ENABLE_CUDA
#include <cuda_runtime.h>
#endif
#if CTP_ENABLE_ROCM
#include <hip/hip_runtime.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum class Target { kPinned, kHbm, kBoth };

struct Opts {
  unsigned int nblocks = 4;
  std::size_t per_block_bytes = 16ULL << 20;
  std::size_t page_bytes = 1ULL << 20;
  Target target = Target::kBoth;
  int iters = 6;
  int threads_per_block = 32;
  bool concurrent_flush = false;
  bool do_read = false;
  int gpu_id = 0;
};

void PrintUsage(const char *prog) {
  std::fprintf(stderr,
      "Usage: %s [options]\n\n"
      "  --blocks N            Grid blocks (default 4)\n"
      "  --per-block-bytes B   Bytes per block (default 16 MiB)\n"
      "  --page-bytes P        Inner-loop page size (default 1 MiB)\n"
      "  --target {pinned,hbm,both}  Buffer kind (default both)\n"
      "  --iters N             Total iters incl warmup (default 6)\n"
      "  --threads-per-block T Threads/block (default 32)\n"
      "  --concurrent-flush    Launch a flush-style kernel concurrently\n"
      "  --read                Measure reads instead of writes\n"
      "  --gpu-id N            GPU index (default 0)\n"
      "  --help                Show this message\n",
      prog);
}

bool ParseOpts(int argc, char *argv[], Opts &o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for %s\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") { PrintUsage(argv[0]); std::exit(0); }
    else if (a == "--blocks") o.nblocks = std::atoi(next("--blocks"));
    else if (a == "--per-block-bytes")
      o.per_block_bytes = std::strtoull(next("--per-block-bytes"), nullptr, 10);
    else if (a == "--page-bytes")
      o.page_bytes = std::strtoull(next("--page-bytes"), nullptr, 10);
    else if (a == "--target") {
      std::string v = next("--target");
      if (v == "pinned") o.target = Target::kPinned;
      else if (v == "hbm") o.target = Target::kHbm;
      else if (v == "both") o.target = Target::kBoth;
      else { std::fprintf(stderr, "Bad target: %s\n", v.c_str()); return false; }
    }
    else if (a == "--iters") o.iters = std::atoi(next("--iters"));
    else if (a == "--threads-per-block")
      o.threads_per_block = std::atoi(next("--threads-per-block"));
    else if (a == "--concurrent-flush") o.concurrent_flush = true;
    else if (a == "--read") o.do_read = true;
    else if (a == "--gpu-id") o.gpu_id = std::atoi(next("--gpu-id"));
    else {
      std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
      PrintUsage(argv[0]);
      return false;
    }
  }
  if (o.per_block_bytes % o.page_bytes != 0) {
    std::fprintf(stderr,
        "per-block-bytes (%zu) must be a multiple of page-bytes (%zu)\n",
        o.per_block_bytes, o.page_bytes);
    return false;
  }
  if (o.iters < 2) {
    std::fprintf(stderr, "iters must be >= 2 (1 warmup + 1 measured)\n");
    return false;
  }
  return true;
}

/**
 * Warp-cooperative coalesced write. Each grid block (== logical buffer
 * block) takes a stride into `buf` and loops over pages, writing
 * `page_bytes` per page as uint4 stores. Mirrors gpu_vector's
 * write_range coalescing pattern (32 lanes × uint4).
 */
__global__ void WriteKernel(char *buf, std::size_t per_block_bytes,
                             std::size_t page_bytes,
                             std::size_t buf_stride_bytes) {
  char *block_base = buf + static_cast<std::size_t>(blockIdx.x) *
                              buf_stride_bytes;
  unsigned int lane = threadIdx.x;
  unsigned int nlanes = blockDim.x;
  std::size_t npages = per_block_bytes / page_bytes;
  for (std::size_t pg = 0; pg < npages; ++pg) {
    char *page_base = block_base + pg * page_bytes;
    // Stride-1 uint4 writes (16B per thread per step).
    std::size_t step = nlanes * sizeof(uint4);
    std::size_t n_steps = page_bytes / step;
    uint4 v = make_uint4(pg, pg + 1, pg + 2, pg + 3);
    for (std::size_t s = 0; s < n_steps; ++s) {
      uint4 *p = reinterpret_cast<uint4 *>(page_base + s * step) + lane;
      *p = v;
    }
  }
}

/**
 * Warp-cooperative coalesced read with a tiny dependent accumulation
 * so the compiler can't elide the load. Writes the per-thread sum to
 * `sink` once at the end (one atomicAdd per grid block).
 */
__global__ void ReadKernel(const char *buf, std::size_t per_block_bytes,
                            std::size_t page_bytes,
                            std::size_t buf_stride_bytes,
                            unsigned long long *sink) {
  const char *block_base = buf + static_cast<std::size_t>(blockIdx.x) *
                                    buf_stride_bytes;
  unsigned int lane = threadIdx.x;
  unsigned int nlanes = blockDim.x;
  std::size_t npages = per_block_bytes / page_bytes;
  unsigned long long acc = 0;
  for (std::size_t pg = 0; pg < npages; ++pg) {
    const char *page_base = block_base + pg * page_bytes;
    std::size_t step = nlanes * sizeof(uint4);
    std::size_t n_steps = page_bytes / step;
    for (std::size_t s = 0; s < n_steps; ++s) {
      const uint4 *p =
          reinterpret_cast<const uint4 *>(page_base + s * step) + lane;
      uint4 v = *p;
      acc += v.x + v.y + v.z + v.w;
    }
  }
  if (lane == 0) atomicAdd(sink, acc);
}

/**
 * Flush-style D2H spammer: a tiny kernel doing many small writes to
 * pinned host, mimicking the manager Phase 3 PutBlob volume so we can
 * see whether the contention slows the main kernel. Each step writes
 * a small descriptor word into a flush sink.
 */
__global__ void FlushSpamKernel(unsigned int *flush_sink,
                                 std::size_t sink_cap,
                                 unsigned int n_writes) {
  unsigned int tid = blockIdx.x * blockDim.x + threadIdx.x;
  for (unsigned int i = 0; i < n_writes; ++i) {
    std::size_t idx = (tid + i * 1024u) % sink_cap;
    flush_sink[idx] = i;
  }
}

double TimeOneIter(const Opts &o, char *buf, std::size_t buf_stride_bytes,
                    unsigned long long *read_sink,
                    cudaStream_t main_stream, cudaStream_t flush_stream,
                    unsigned int *flush_sink, std::size_t flush_sink_cap) {
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
  if (o.concurrent_flush) {
    // Spam ~64 small writes per main-iter, on a non-blocking stream.
    FlushSpamKernel<<<32, 32, 0, flush_stream>>>(
        flush_sink, flush_sink_cap, 64u);
  }
  if (o.do_read) {
    ReadKernel<<<o.nblocks, o.threads_per_block, 0, main_stream>>>(
        buf, o.per_block_bytes, o.page_bytes, buf_stride_bytes, read_sink);
  } else {
    WriteKernel<<<o.nblocks, o.threads_per_block, 0, main_stream>>>(
        buf, o.per_block_bytes, o.page_bytes, buf_stride_bytes);
  }
  cudaStreamSynchronize(main_stream);
  if (o.concurrent_flush) cudaStreamSynchronize(flush_stream);
  auto t1 = clk::now();
  return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

void RunTarget(const Opts &o, Target tgt) {
  const char *tgt_name = (tgt == Target::kPinned) ? "pinned" : "hbm";
  std::size_t buf_stride_bytes = o.per_block_bytes;  // contiguous
  std::size_t total_bytes =
      static_cast<std::size_t>(o.nblocks) * o.per_block_bytes;

  char *buf = nullptr;
  if (tgt == Target::kPinned) {
    buf = ctp::GpuApi::MallocHost<char>(total_bytes);
  } else {
    buf = ctp::GpuApi::Malloc<char>(total_bytes);
  }
  if (!buf) {
    std::fprintf(stderr, "Allocation failed for %zu bytes on %s\n",
                 total_bytes, tgt_name);
    return;
  }
  // Pre-fault pinned pages so first-iter doesn't pay first-touch cost.
  if (tgt == Target::kPinned) {
    std::memset(buf, 0, total_bytes);
  } else {
    cudaMemset(buf, 0, total_bytes);
    cudaDeviceSynchronize();
  }

  unsigned long long *read_sink = nullptr;
  cudaMalloc(&read_sink, sizeof(unsigned long long));
  cudaMemset(read_sink, 0, sizeof(unsigned long long));

  cudaStream_t main_stream = nullptr;
  cudaStreamCreateWithFlags(&main_stream, cudaStreamNonBlocking);
  cudaStream_t flush_stream = nullptr;
  unsigned int *flush_sink = nullptr;
  std::size_t flush_sink_cap = 0;
  if (o.concurrent_flush) {
    cudaStreamCreateWithFlags(&flush_stream, cudaStreamNonBlocking);
    flush_sink_cap = 64 * 1024;
    flush_sink = ctp::GpuApi::MallocHost<unsigned int>(flush_sink_cap);
    std::memset(flush_sink, 0, flush_sink_cap * sizeof(unsigned int));
  }

  std::vector<double> times;
  times.reserve(o.iters);
  for (int it = 0; it < o.iters; ++it) {
    double us = TimeOneIter(o, buf, buf_stride_bytes, read_sink,
                             main_stream, flush_stream,
                             flush_sink, flush_sink_cap);
    if (it > 0) times.push_back(us);  // skip warmup
  }
  std::sort(times.begin(), times.end());
  double tmin = times.front();
  double tmed = times[times.size() / 2];
  double tmax = times.back();
  double tavg = 0;
  for (double v : times) tavg += v;
  tavg /= times.size();

  double mib_total = total_bytes / static_cast<double>(1ULL << 20);
  double bw_med_gibs = (tmed > 0)
      ? (mib_total / (tmed / 1e6)) / 1024.0 : 0.0;
  double bw_min_gibs = (tmin > 0)
      ? (mib_total / (tmin / 1e6)) / 1024.0 : 0.0;

  const char *op = o.do_read ? "read" : "write";
  std::printf(
      "  %-7s %-6s  min=%.2f med=%.2f avg=%.2f max=%.2f ms"
      "  med=%6.2f GiB/s  peak=%6.2f GiB/s%s\n",
      tgt_name, op,
      tmin / 1000.0, tmed / 1000.0, tavg / 1000.0, tmax / 1000.0,
      bw_med_gibs, bw_min_gibs,
      o.concurrent_flush ? "  (+flush)" : "");

  // Cleanup.
  cudaStreamDestroy(main_stream);
  if (flush_stream) cudaStreamDestroy(flush_stream);
  if (flush_sink) ctp::GpuApi::FreeHost(flush_sink);
  cudaFree(read_sink);
  if (tgt == Target::kPinned) {
    ctp::GpuApi::FreeHost(buf);
  } else {
    ctp::GpuApi::Free(buf);
  }
}

}  // namespace

int main(int argc, char *argv[]) {
  Opts o;
  if (!ParseOpts(argc, argv, o)) return 2;

  ctp::GpuApi::SetDevice(o.gpu_id);

  std::printf(
      "[KERN-BENCH] blocks=%u per_block=%.2f MiB page=%.2f MiB total=%.2f MiB "
      "threads/block=%d iters=%d op=%s concurrent_flush=%s\n",
      o.nblocks,
      o.per_block_bytes / (double)(1ULL << 20),
      o.page_bytes / (double)(1ULL << 20),
      o.nblocks * o.per_block_bytes / (double)(1ULL << 20),
      o.threads_per_block, o.iters,
      o.do_read ? "read" : "write",
      o.concurrent_flush ? "yes" : "no");

  if (o.target == Target::kHbm || o.target == Target::kBoth) {
    RunTarget(o, Target::kHbm);
  }
  if (o.target == Target::kPinned || o.target == Target::kBoth) {
    RunTarget(o, Target::kPinned);
  }
  return 0;
}

#else

int main() { return 0; }

#endif  // (CTP_ENABLE_CUDA || CTP_ENABLE_ROCM) && !CTP_ENABLE_SYCL

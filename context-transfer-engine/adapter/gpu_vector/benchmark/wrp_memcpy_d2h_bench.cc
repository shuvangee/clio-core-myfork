/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

/**
 * Baseline cudaMemcpy benchmark.
 *
 * Measures raw device↔host transfer cost across a sweep of I/O sizes
 * so we have a lower bound to compare gpu_vector PutBlob/GetBlob
 * latency against. The producer-only worker path is bounded by:
 *
 *   per_put_latency >= D2H_memcpy(POD task) + D2H_memcpy(blob_data) +
 *                      H2D_memcpy(POD task) + cudaMemcpy(flag word)
 *
 * Anything beyond that is runtime overhead — coroutine resumption,
 * lane scheduling, bdev allocate, etc.
 *
 * Knobs:
 *   --direction {d2h,h2d,d2d,h2h}  Copy direction (default d2h)
 *   --pinned / --pageable          Host buffer kind (default pinned)
 *   --async / --sync               cudaMemcpyAsync vs cudaMemcpy
 *                                  (default sync)
 *   --stream {default,nonblocking} Async stream kind (default
 *                                  nonblocking)
 *   --io-sizes "S1,S2,..."          Comma-separated sizes in bytes,
 *                                  OR
 *   --io-size N                    A single size (overrides sweep)
 *   --iters N                      Inner repeats per size (default
 *                                  100; first iter is warm-up)
 *   --warmup N                     Warm-up iterations excluded from
 *                                  stats (default 5)
 */

#if (HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM) && !HSHM_ENABLE_SYCL

#include <hermes_shm/util/gpu_api.h>

#if HSHM_ENABLE_CUDA
#include <cuda_runtime.h>
#endif
#if HSHM_ENABLE_ROCM
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

enum class Dir { D2H, H2D, D2D, H2H };

struct Opts {
  Dir direction = Dir::D2H;
  bool pinned = true;
  bool async = false;
  bool stream_nonblocking = true;  // only used with --async
  std::vector<std::size_t> sizes;
  std::size_t single_size = 0;
  int iters = 100;
  int warmup = 5;
  int gpu_id = 0;
};

const std::vector<std::size_t> kDefaultSizes = {
    4ULL * 1024,        // 4 KiB
    16ULL * 1024,       // 16 KiB
    64ULL * 1024,       // 64 KiB
    256ULL * 1024,      // 256 KiB
    1ULL * 1024 * 1024, // 1 MiB
    4ULL * 1024 * 1024, // 4 MiB
    16ULL * 1024 * 1024, // 16 MiB
    64ULL * 1024 * 1024, // 64 MiB
};

void PrintUsage(const char *prog) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "\n"
               "  --direction {d2h,h2d,d2d,h2h}  Copy direction (default d2h)\n"
               "  --pinned                       Use cudaMallocHost host buf (default)\n"
               "  --pageable                     Use plain malloc host buf\n"
               "  --async                        cudaMemcpyAsync + stream sync\n"
               "  --sync                         Plain cudaMemcpy (default)\n"
               "  --stream {default,nonblocking} Stream kind for --async (default nonblocking)\n"
               "  --io-size BYTES                Single I/O size (skips sweep)\n"
               "  --io-sizes \"S1,S2,...\"          Comma-separated sweep sizes in bytes\n"
               "  --iters N                      Repeats per size (default 100)\n"
               "  --warmup N                     Warm-up iters excluded from stats (default 5)\n"
               "  --gpu-id N                     GPU index (default 0)\n"
               "  --help                         Show this message\n",
               prog);
}

bool ParseSizesCsv(const char *csv, std::vector<std::size_t> &out) {
  out.clear();
  std::string s = csv;
  std::size_t start = 0;
  while (start <= s.size()) {
    std::size_t comma = s.find(',', start);
    std::string tok = s.substr(start, comma - start);
    if (!tok.empty()) {
      char *end = nullptr;
      unsigned long long v = std::strtoull(tok.c_str(), &end, 10);
      if (end == tok.c_str() || v == 0) {
        std::fprintf(stderr, "Bad size token: '%s'\n", tok.c_str());
        return false;
      }
      out.push_back(static_cast<std::size_t>(v));
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return !out.empty();
}

bool ParseOpts(int argc, char *argv[], Opts &opts) {
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
    else if (a == "--direction") {
      std::string v = next("--direction");
      if (v == "d2h") opts.direction = Dir::D2H;
      else if (v == "h2d") opts.direction = Dir::H2D;
      else if (v == "d2d") opts.direction = Dir::D2D;
      else if (v == "h2h") opts.direction = Dir::H2H;
      else { std::fprintf(stderr, "Bad direction: %s\n", v.c_str()); return false; }
    }
    else if (a == "--pinned") opts.pinned = true;
    else if (a == "--pageable") opts.pinned = false;
    else if (a == "--async") opts.async = true;
    else if (a == "--sync") opts.async = false;
    else if (a == "--stream") {
      std::string v = next("--stream");
      if (v == "default") opts.stream_nonblocking = false;
      else if (v == "nonblocking") opts.stream_nonblocking = true;
      else { std::fprintf(stderr, "Bad stream: %s\n", v.c_str()); return false; }
    }
    else if (a == "--io-size")
      opts.single_size = std::strtoull(next("--io-size"), nullptr, 10);
    else if (a == "--io-sizes") {
      if (!ParseSizesCsv(next("--io-sizes"), opts.sizes)) return false;
    }
    else if (a == "--iters") opts.iters = std::atoi(next("--iters"));
    else if (a == "--warmup") opts.warmup = std::atoi(next("--warmup"));
    else if (a == "--gpu-id") opts.gpu_id = std::atoi(next("--gpu-id"));
    else {
      std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
      PrintUsage(argv[0]);
      return false;
    }
  }
  if (opts.iters <= opts.warmup) {
    std::fprintf(stderr, "iters (%d) must be > warmup (%d)\n",
                 opts.iters, opts.warmup);
    return false;
  }
  return true;
}

const char *DirName(Dir d) {
  switch (d) {
    case Dir::D2H: return "D2H";
    case Dir::H2D: return "H2D";
    case Dir::D2D: return "D2D";
    case Dir::H2H: return "H2H";
  }
  return "?";
}

#if HSHM_ENABLE_CUDA
cudaMemcpyKind ToCudaKind(Dir d) {
  switch (d) {
    case Dir::D2H: return cudaMemcpyDeviceToHost;
    case Dir::H2D: return cudaMemcpyHostToDevice;
    case Dir::D2D: return cudaMemcpyDeviceToDevice;
    case Dir::H2H: return cudaMemcpyHostToHost;
  }
  return cudaMemcpyDefault;
}
#elif HSHM_ENABLE_ROCM
hipMemcpyKind ToHipKind(Dir d) {
  switch (d) {
    case Dir::D2H: return hipMemcpyDeviceToHost;
    case Dir::H2D: return hipMemcpyHostToDevice;
    case Dir::D2D: return hipMemcpyDeviceToDevice;
    case Dir::H2H: return hipMemcpyHostToHost;
  }
  return hipMemcpyDefault;
}
#endif

/**
 * Allocate src/dst pair for the given direction. Sizes are in bytes.
 * pinned controls the host-side allocator (kHostMalloc vs cudaMallocHost).
 */
struct BufPair {
  void *src = nullptr;
  void *dst = nullptr;
};

BufPair AllocPair(Dir d, std::size_t bytes, bool pinned) {
  BufPair bp{};
  auto alloc_host = [&](void **out) {
    if (pinned) {
      *out = hshm::GpuApi::MallocHost<char>(bytes);
    } else {
      *out = std::malloc(bytes);
      if (*out) std::memset(*out, 0xA5, bytes);
    }
  };
  auto alloc_dev = [&](void **out) {
    *out = hshm::GpuApi::Malloc<char>(bytes);
  };
  switch (d) {
    case Dir::D2H: alloc_dev(&bp.src); alloc_host(&bp.dst); break;
    case Dir::H2D: alloc_host(&bp.src); alloc_dev(&bp.dst); break;
    case Dir::D2D: alloc_dev(&bp.src); alloc_dev(&bp.dst); break;
    case Dir::H2H: alloc_host(&bp.src); alloc_host(&bp.dst); break;
  }
  if (!bp.src || !bp.dst) {
    std::fprintf(stderr, "Allocation failed for %zu bytes\n", bytes);
    std::exit(2);
  }
  return bp;
}

void FreePair(Dir d, BufPair &bp, bool pinned) {
  auto free_host = [&](void *p) {
    if (pinned) hshm::GpuApi::FreeHost(static_cast<char *>(p));
    else std::free(p);
  };
  auto free_dev = [&](void *p) {
    hshm::GpuApi::Free(static_cast<char *>(p));
  };
  switch (d) {
    case Dir::D2H: free_dev(bp.src); free_host(bp.dst); break;
    case Dir::H2D: free_host(bp.src); free_dev(bp.dst); break;
    case Dir::D2D: free_dev(bp.src); free_dev(bp.dst); break;
    case Dir::H2H: free_host(bp.src); free_host(bp.dst); break;
  }
}

/** Issue one transfer and synchronize, returning microseconds spent. */
double TimeOneCopy(Dir d, void *dst, const void *src, std::size_t bytes,
                    bool async, void *stream) {
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
#if HSHM_ENABLE_CUDA
  if (async) {
    cudaMemcpyAsync(dst, src, bytes, ToCudaKind(d),
                    static_cast<cudaStream_t>(stream));
    cudaStreamSynchronize(static_cast<cudaStream_t>(stream));
  } else {
    cudaMemcpy(dst, src, bytes, ToCudaKind(d));
  }
#elif HSHM_ENABLE_ROCM
  if (async) {
    hipMemcpyAsync(dst, src, bytes, ToHipKind(d),
                   static_cast<hipStream_t>(stream));
    hipStreamSynchronize(static_cast<hipStream_t>(stream));
  } else {
    hipMemcpy(dst, src, bytes, ToHipKind(d));
  }
#else
  std::memcpy(dst, src, bytes);
#endif
  auto t1 = clk::now();
  return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

void RunOne(const Opts &opts, std::size_t bytes, void *stream) {
  BufPair bp = AllocPair(opts.direction, bytes, opts.pinned);
  // Touch pages so first-iter doesn't pay alloc-on-fault penalty for
  // pageable host buffers.
  if (opts.direction == Dir::H2D || opts.direction == Dir::H2H) {
    std::memset(bp.src, 0xA5, bytes);
  }

  std::vector<double> times;
  times.reserve(opts.iters);
  for (int it = 0; it < opts.iters; ++it) {
    double us = TimeOneCopy(opts.direction, bp.dst, bp.src, bytes,
                             opts.async, stream);
    if (it >= opts.warmup) times.push_back(us);
  }
  std::sort(times.begin(), times.end());
  double tmin = times.front();
  double tmed = times[times.size() / 2];
  double tmax = times.back();
  double tsum = 0;
  for (double v : times) tsum += v;
  double tavg = tsum / times.size();

  // Bandwidth from median (steady-state) and from min (peak).
  double mib = bytes / static_cast<double>(1ULL << 20);
  double bw_med = (tmed > 0) ? (mib / (tmed / 1e6)) / 1024.0 : 0.0;  // GiB/s
  double bw_min = (tmin > 0) ? (mib / (tmin / 1e6)) / 1024.0 : 0.0;

  // Format size label.
  char size_lbl[32];
  if (bytes >= (1ULL << 30))
    std::snprintf(size_lbl, sizeof(size_lbl), "%.1f GiB",
                  bytes / static_cast<double>(1ULL << 30));
  else if (bytes >= (1ULL << 20))
    std::snprintf(size_lbl, sizeof(size_lbl), "%.0f MiB",
                  bytes / static_cast<double>(1ULL << 20));
  else if (bytes >= (1ULL << 10))
    std::snprintf(size_lbl, sizeof(size_lbl), "%.0f KiB",
                  bytes / static_cast<double>(1ULL << 10));
  else
    std::snprintf(size_lbl, sizeof(size_lbl), "%zu B", bytes);

  std::printf("  %-10s  %8.2f  %8.2f  %8.2f  %8.2f   %6.2f   %6.2f\n",
              size_lbl, tmin, tmed, tavg, tmax, bw_med, bw_min);

  FreePair(opts.direction, bp, opts.pinned);
}

}  // namespace

int main(int argc, char *argv[]) {
  Opts opts;
  if (!ParseOpts(argc, argv, opts)) return 2;
  if (opts.single_size > 0) opts.sizes = {opts.single_size};
  else if (opts.sizes.empty()) opts.sizes = kDefaultSizes;

  hshm::GpuApi::SetDevice(opts.gpu_id);

  void *stream = nullptr;
#if HSHM_ENABLE_CUDA
  if (opts.async) {
    cudaStream_t s = nullptr;
    if (opts.stream_nonblocking) {
      cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking);
    } else {
      // legacy default stream (0) — implicit sync with all other streams
      s = nullptr;
    }
    stream = static_cast<void *>(s);
  }
#elif HSHM_ENABLE_ROCM
  if (opts.async) {
    hipStream_t s = nullptr;
    if (opts.stream_nonblocking) {
      hipStreamCreateWithFlags(&s, hipStreamNonBlocking);
    }
    stream = static_cast<void *>(s);
  }
#endif

  std::printf(
      "[BENCH] memcpy %s host=%s mode=%s%s%s gpu_id=%d iters=%d "
      "(warmup=%d)\n",
      DirName(opts.direction), opts.pinned ? "pinned" : "pageable",
      opts.async ? "async" : "sync",
      opts.async ? " stream=" : "",
      opts.async ? (opts.stream_nonblocking ? "nonblocking" : "default") : "",
      opts.gpu_id, opts.iters, opts.warmup);
  std::printf("  %-10s  %8s  %8s  %8s  %8s   %6s   %6s\n",
              "size", "min(us)", "med(us)", "avg(us)", "max(us)",
              "GiB/s", "peak");
  std::printf("  %-10s  %8s  %8s  %8s  %8s   %6s   %6s\n",
              "----", "-------", "-------", "-------", "-------",
              "-----", "----");
  for (std::size_t bytes : opts.sizes) {
    RunOne(opts, bytes, stream);
  }

#if HSHM_ENABLE_CUDA
  if (opts.async && stream && opts.stream_nonblocking) {
    cudaStreamDestroy(static_cast<cudaStream_t>(stream));
  }
#elif HSHM_ENABLE_ROCM
  if (opts.async && stream && opts.stream_nonblocking) {
    hipStreamDestroy(static_cast<hipStream_t>(stream));
  }
#endif
  return 0;
}

#else

int main() { return 0; }

#endif  // (HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM) && !HSHM_ENABLE_SYCL

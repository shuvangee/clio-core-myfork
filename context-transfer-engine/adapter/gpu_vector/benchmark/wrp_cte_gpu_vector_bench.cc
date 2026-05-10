/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

/**
 * gpu_vector performance benchmark.
 *
 * Drives a Vector<chi::u32> through a write/flush/read cycle while
 * exposing every interesting parameter on the command line:
 *
 *   --blocks N             Number of cache blocks (default 32)
 *   --pages-per-block P    Cache pages per block (default 2)
 *   --page-size B          Page size in bytes (default 1048576 = 1 MiB)
 *   --ratio R              Total data = R × cache size; controls how
 *                          much oversubscription each block sees
 *                          (default 1.0 → no eviction; > 1 forces
 *                          eviction; defaults to 1.0).
 *   --total-bytes B        Override total data size in bytes; if set
 *                          this takes precedence over --ratio.
 *   --cache-period-ms N    Period of the periodic CacheMgmtKernel
 *                          (0 = disabled; default 0).
 *   --iters N              Repeat the write/flush/read cycle N times
 *                          (default 1).
 *   --no-read              Skip the read phase (only measure write +
 *                          flush). Useful when you want a clean write-
 *                          path number without faulting overhead.
 *   --bdev-capacity-mib N  kRam bdev target capacity in MiB
 *                          (default = 4× total_bytes, min 64).
 *   --gpu-id N             GPU index (default 0).
 *
 * Output: per-iteration timings (write/flush/read in ms) and bandwidth
 * (MiB/s), plus a final summary with min/median/max across iters. Per-
 * task latency is computed as wall_time / N_tasks where N_tasks is the
 * expected count given the geometry.
 */

#if (HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM) && !HSHM_ENABLE_SYCL

#include <chimaera/bdev/bdev_client.h>
#include <chimaera/chimaera.h>
#include <chimaera/gpu/gpu_info.h>
#include <chimaera/gpu/gpu_ipc_manager.h>
#include <chimaera/singletons.h>
#include <chimaera/types.h>
#include <hermes_shm/util/gpu_api.h>
#include <wrp_cte/core/core_client.h>
#include <wrp_cte/core/core_tasks.h>
#include <wrp_cte/gpu_vector/gpu_vector.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace gv = wrp_cte::gpu_vector;
namespace dev = cte::gpu::dev;

namespace {

struct BenchOpts {
  chi::u32 nblocks = 32;
  chi::u32 pages_per_block = 2;
  chi::u64 page_size = 1ULL << 20;  // 1 MiB
  double ratio = 1.0;
  chi::u64 total_bytes = 0;          // 0 = derive from ratio
  chi::u64 per_block_bytes = 0;      // 0 = derive from ratio / total_bytes
  chi::u32 cache_period_ms = 0;
  chi::u32 iters = 1;
  bool do_read = true;
  chi::u32 gpu_id = 0;
  chi::u64 bdev_capacity_mib = 0;    // 0 = auto
};

void PrintUsage(const char *prog) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "\n"
               "  --blocks N             Cache blocks (default 32)\n"
               "  --pages-per-block P    Pages per cache block (default 2)\n"
               "  --page-size BYTES      Page size in bytes (default 1048576)\n"
               "  --per-block-bytes B    Bytes each block writes/reads (overrides --ratio\n"
               "                         and --total-bytes; total_bytes = blocks * B).\n"
               "                         Use this to amortize kernel-launch overhead\n"
               "                         (e.g. --per-block-bytes 16777216 = 16 MiB/block).\n"
               "  --ratio R              total_bytes = R * cache_bytes (default 1.0)\n"
               "  --total-bytes BYTES    Override total bytes (takes precedence over --ratio)\n"
               "  --cache-period-ms N    Periodic CacheMgmtKernel period (0=off, default 0)\n"
               "  --iters N              Repeat write/flush/read cycle N times (default 1)\n"
               "  --no-read              Skip read+verify phase\n"
               "  --bdev-capacity-mib N  kRam bdev capacity (default = 4x total_bytes, min 64)\n"
               "  --gpu-id N             GPU index (default 0)\n"
               "  --help                 Show this message\n",
               prog);
}

bool ParseOpts(int argc, char *argv[], BenchOpts &opts) {
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
    else if (a == "--blocks") opts.nblocks = std::atoi(next("--blocks"));
    else if (a == "--pages-per-block")
      opts.pages_per_block = std::atoi(next("--pages-per-block"));
    else if (a == "--page-size")
      opts.page_size = std::strtoull(next("--page-size"), nullptr, 10);
    else if (a == "--ratio")
      opts.ratio = std::atof(next("--ratio"));
    else if (a == "--total-bytes")
      opts.total_bytes = std::strtoull(next("--total-bytes"), nullptr, 10);
    else if (a == "--per-block-bytes")
      opts.per_block_bytes =
          std::strtoull(next("--per-block-bytes"), nullptr, 10);
    else if (a == "--cache-period-ms")
      opts.cache_period_ms = std::atoi(next("--cache-period-ms"));
    else if (a == "--iters")
      opts.iters = std::atoi(next("--iters"));
    else if (a == "--no-read") opts.do_read = false;
    else if (a == "--bdev-capacity-mib")
      opts.bdev_capacity_mib =
          std::strtoull(next("--bdev-capacity-mib"), nullptr, 10);
    else if (a == "--gpu-id") opts.gpu_id = std::atoi(next("--gpu-id"));
    else {
      std::fprintf(stderr, "Unknown arg: %s\n", a.c_str());
      PrintUsage(argv[0]);
      return false;
    }
  }
  return true;
}

/** One-shot Chimaera + CTE pool + kRam bdev target setup. Host-only:
 *  AsyncCreate / AsyncRegisterTarget aren't visible in the GPU device
 *  pass (nvcc parses both passes for __global__-bearing TUs). */
#if !HSHM_IS_DEVICE_PASS
void EnsureInit(const BenchOpts &opts, chi::u64 bdev_capacity_bytes) {
  std::fprintf(stderr, "[INIT] Starting Chimaera server\n");
  if (!chi::CHIMAERA_INIT(chi::ChimaeraMode::kServer)) {
    std::fprintf(stderr, "[INIT] CHIMAERA_INIT failed\n");
    std::exit(2);
  }
  if (!wrp_cte::core::WRP_CTE_CLIENT_INIT()) {
    std::fprintf(stderr, "[INIT] WRP_CTE_CLIENT_INIT failed\n");
    std::exit(2);
  }
  auto *cte_client = WRP_CTE_CLIENT;
  cte_client->Init(wrp_cte::core::kCtePoolId);
  wrp_cte::core::CreateParams params;
  auto create_task = cte_client->AsyncCreate(
      chi::PoolQuery::Dynamic(), wrp_cte::core::kCtePoolName,
      wrp_cte::core::kCtePoolId, params);
  create_task.Wait();
  if (create_task->GetReturnCode() != 0) {
    std::fprintf(stderr, "[INIT] CTE pool create failed rc=%u\n",
                 create_task->GetReturnCode());
    std::exit(2);
  }
  std::this_thread::sleep_for(50ms);

  chi::PoolId bdev_pool_id(951, 0);
  chimaera::bdev::Client bdev_client(bdev_pool_id);
  auto bdev_create = bdev_client.AsyncCreate(
      chi::PoolQuery::Dynamic(), std::string("gpu_vector_bench_ram"),
      bdev_pool_id, chimaera::bdev::BdevType::kRam, bdev_capacity_bytes);
  bdev_create.Wait();
  if (bdev_create->GetReturnCode() != 0) {
    std::fprintf(stderr, "[INIT] bdev create failed rc=%u\n",
                 bdev_create->GetReturnCode());
    std::exit(2);
  }
  std::this_thread::sleep_for(50ms);
  auto reg_task = cte_client->AsyncRegisterTarget(
      "gpu_vector_bench_ram", chimaera::bdev::BdevType::kRam,
      bdev_capacity_bytes, chi::PoolQuery::Local(), bdev_pool_id);
  reg_task.Wait();
  if (reg_task->GetReturnCode() != 0) {
    std::fprintf(stderr, "[INIT] RegisterTarget failed rc=%u\n",
                 reg_task->GetReturnCode());
    std::exit(2);
  }
  std::this_thread::sleep_for(50ms);
  (void)opts;
}
#endif  // !HSHM_IS_DEVICE_PASS

/** Compute the number of expected PutBlobs for the given geometry. */
chi::u64 ExpectedPuts(chi::u32 nblocks, chi::u32 pages_per_block,
                       chi::u64 per_block_pages) {
  if (per_block_pages <= pages_per_block) {
    // No eviction; only the final flush of dirty pages contributes.
    return static_cast<chi::u64>(nblocks) * per_block_pages;
  }
  // 1 eviction per page beyond the cache; evictions push 1 put per
  // dirty page, plus pages_per_block final-flush puts at FlushAllSync.
  chi::u64 evict_per_block = per_block_pages - pages_per_block;
  return static_cast<chi::u64>(nblocks) *
         (evict_per_block + pages_per_block);
}

/** Compute the number of expected GetBlobs (sequential read pattern):
 *  one fault per page when the stripe exceeds the cache. */
chi::u64 ExpectedGets(chi::u32 nblocks, chi::u32 pages_per_block,
                       chi::u64 per_block_pages) {
  if (per_block_pages <= pages_per_block) return 0;  // hits cache
  return static_cast<chi::u64>(nblocks) * per_block_pages;
}

}  // namespace

/** Sequential write of v[i] = i across each block's stripe. */
__global__ void BenchWriteKernel(chi::IpcManagerGpuInfo info,
                                  gv::DeviceView<chi::u32> view,
                                  chi::u64 per_block) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  dev::vector<chi::u32> v(view, g_ipc_manager_ptr);
  if (threadIdx.x != 0) return;
  chi::u64 lo = static_cast<chi::u64>(blockIdx.x) * per_block;
  chi::u64 hi = lo + per_block;
  for (chi::u64 i = lo; i < hi; ++i) {
    v[i] = static_cast<chi::u32>(i);
  }
  (void)g_ipc_manager;
}

/** Sequential read into a result buffer (host-pinned). */
__global__ void BenchReadKernel(chi::IpcManagerGpuInfo info,
                                 gv::DeviceView<chi::u32> view,
                                 chi::u32 *result, chi::u64 per_block) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  dev::vector<chi::u32> v(view, g_ipc_manager_ptr);
  if (threadIdx.x != 0) return;
  chi::u64 lo = static_cast<chi::u64>(blockIdx.x) * per_block;
  chi::u64 hi = lo + per_block;
  for (chi::u64 i = lo; i < hi; ++i) {
    result[i] = v[i];
  }
  (void)g_ipc_manager;
}

#if !HSHM_IS_DEVICE_PASS

/** Format a byte count as a human-friendly MiB / GiB string. */
std::string FmtBytes(chi::u64 b) {
  char buf[64];
  if (b >= (1ULL << 30)) {
    std::snprintf(buf, sizeof(buf), "%.2f GiB", b / static_cast<double>(1ULL << 30));
  } else if (b >= (1ULL << 20)) {
    std::snprintf(buf, sizeof(buf), "%.2f MiB", b / static_cast<double>(1ULL << 20));
  } else if (b >= (1ULL << 10)) {
    std::snprintf(buf, sizeof(buf), "%.2f KiB", b / static_cast<double>(1ULL << 10));
  } else {
    std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
  }
  return std::string(buf);
}

int main(int argc, char *argv[]) {
  BenchOpts opts;
  if (!ParseOpts(argc, argv, opts)) return 2;
  if (opts.nblocks == 0 || opts.pages_per_block == 0 ||
      opts.page_size == 0) {
    std::fprintf(stderr, "blocks/pages/page_size must be > 0\n");
    return 2;
  }
  if (opts.page_size % sizeof(chi::u32) != 0) {
    std::fprintf(stderr, "page_size must be a multiple of sizeof(u32)\n");
    return 2;
  }

  chi::u64 cache_bytes = static_cast<chi::u64>(opts.nblocks) *
                         opts.pages_per_block * opts.page_size;
  // Resolve in priority: --per-block-bytes > --total-bytes > --ratio.
  chi::u64 per_block_bytes;
  if (opts.per_block_bytes > 0) {
    per_block_bytes = opts.per_block_bytes;
  } else if (opts.total_bytes > 0) {
    per_block_bytes = opts.total_bytes / opts.nblocks;
  } else {
    chi::u64 t = static_cast<chi::u64>(opts.ratio * cache_bytes);
    if (t < opts.page_size) t = opts.page_size;
    per_block_bytes = t / opts.nblocks;
  }
  // Round per_block_bytes up to a whole number of pages so the kernel
  // doesn't have to handle a partial-page tail.
  if (per_block_bytes < opts.page_size) per_block_bytes = opts.page_size;
  if (per_block_bytes % opts.page_size != 0) {
    per_block_bytes = ((per_block_bytes + opts.page_size - 1) /
                       opts.page_size) * opts.page_size;
  }
  opts.total_bytes = per_block_bytes * opts.nblocks;
  chi::u64 per_block_pages = per_block_bytes / opts.page_size;
  chi::u64 total_elems = opts.total_bytes / sizeof(chi::u32);
  chi::u64 per_block_elems = per_block_bytes / sizeof(chi::u32);

  chi::u64 bdev_capacity_bytes;
  if (opts.bdev_capacity_mib > 0) {
    bdev_capacity_bytes = opts.bdev_capacity_mib * (1ULL << 20);
  } else {
    bdev_capacity_bytes =
        std::max<chi::u64>(64ULL << 20, opts.total_bytes * 4);
  }

  chi::u64 expected_puts =
      ExpectedPuts(opts.nblocks, opts.pages_per_block, per_block_pages);
  chi::u64 expected_gets =
      ExpectedGets(opts.nblocks, opts.pages_per_block, per_block_pages);

  std::fprintf(stderr,
               "[BENCH] blocks=%u pages_per_block=%u page_size=%s\n"
               "[BENCH] cache=%s total=%s ratio=%.2fx\n"
               "[BENCH] per_block=%llu pages (%s)\n"
               "[BENCH] expected_puts=%llu expected_gets=%llu\n"
               "[BENCH] cache_period_ms=%u iters=%u read=%s\n",
               opts.nblocks, opts.pages_per_block,
               FmtBytes(opts.page_size).c_str(),
               FmtBytes(cache_bytes).c_str(),
               FmtBytes(opts.total_bytes).c_str(),
               opts.total_bytes / static_cast<double>(cache_bytes),
               (unsigned long long)per_block_pages,
               FmtBytes(per_block_bytes).c_str(),
               (unsigned long long)expected_puts,
               (unsigned long long)expected_gets,
               opts.cache_period_ms, opts.iters,
               opts.do_read ? "yes" : "no");

  EnsureInit(opts, bdev_capacity_bytes);
  auto *ipc = CHI_CPU_IPC;

  using clock = std::chrono::steady_clock;
  auto us_since = [](clock::time_point t0) -> long long {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               clock::now() - t0)
        .count();
  };

  std::vector<long long> write_us;
  std::vector<long long> flush_us;
  std::vector<long long> read_us;
  write_us.reserve(opts.iters);
  flush_us.reserve(opts.iters);
  read_us.reserve(opts.iters);

  for (chi::u32 it = 0; it < opts.iters; ++it) {
    // Fresh Vector per iteration to avoid cumulative cache state.
    std::string tag_name = "bench_iter_" + std::to_string(it);
    gv::Vector<chi::u32> vec(tag_name, opts.nblocks, opts.gpu_id,
                              opts.pages_per_block, opts.page_size,
                              opts.cache_period_ms);
    auto view = vec.Device();
    chi::IpcManagerGpuInfo gpu_info =
        ipc->GetGpuIpcManager()->GetGpuInfo(opts.gpu_id);

    // ----- Write (kernel launch + entire kernel duration + cudaSync) -----
    auto t0 = clock::now();
    BenchWriteKernel<<<opts.nblocks, 32>>>(gpu_info, view, per_block_elems);
    hshm::GpuApi::Synchronize();
    long long w = us_since(t0);

    // ----- Flush (CacheMgmtKernel + DrainKernel; this is where the real
    //              PutBlob round-trips happen at ratio=1.0) -----
    auto t1 = clock::now();
    vec.FlushAllSync();
    long long f = us_since(t1);

    long long r = 0;
    if (opts.do_read) {
      // hshm::GpuApi::MallocHost takes BYTES (not elements) — the
      // template parameter is just the cast type for the returned ptr.
      auto *result = hshm::GpuApi::MallocHost<chi::u32>(
          total_elems * sizeof(chi::u32));
      if (!result) {
        std::fprintf(stderr, "MallocHost(%llu B) returned nullptr\n",
                     (unsigned long long)(total_elems * sizeof(chi::u32)));
        std::exit(2);
      }
      std::memset(result, 0, total_elems * sizeof(chi::u32));
      auto t2 = clock::now();
      BenchReadKernel<<<opts.nblocks, 32>>>(gpu_info, view, result,
                                              per_block_elems);
      hshm::GpuApi::Synchronize();
      r = us_since(t2);
      hshm::GpuApi::FreeHost(result);
    }

    write_us.push_back(w);
    flush_us.push_back(f);
    read_us.push_back(r);

    auto bw = [&](long long us) {
      if (us <= 0) return 0.0;
      return (opts.total_bytes / static_cast<double>(1ULL << 20)) /
             (us / 1e6);
    };
    std::fprintf(stderr,
                 "[ITER %u/%u] write=%.3f ms (%.1f MiB/s) "
                 "flush=%.3f ms read=%.3f ms (%.1f MiB/s) "
                 "write+flush=%.1f MiB/s\n",
                 it + 1, opts.iters,
                 w / 1e3, bw(w),
                 f / 1e3, r / 1e3,
                 opts.do_read ? bw(r) : 0.0,
                 bw(w + f));
  }

  // ----- Summary -----
  auto stat = [&](std::vector<long long> &v) {
    if (v.empty()) return std::tuple<long long, long long, long long>(0, 0, 0);
    std::sort(v.begin(), v.end());
    return std::tuple<long long, long long, long long>(
        v.front(), v[v.size() / 2], v.back());
  };
  auto [wmin, wmed, wmax] = stat(write_us);
  auto [fmin, fmed, fmax] = stat(flush_us);
  auto [rmin, rmed, rmax] = stat(read_us);
  auto bw_med = [&](long long us) {
    if (us <= 0) return 0.0;
    return (opts.total_bytes / static_cast<double>(1ULL << 20)) /
           (us / 1e6);
  };

  std::fprintf(stderr,
               "\n[SUMMARY] write : min=%.3f med=%.3f max=%.3f ms "
               "(median %.1f MiB/s, %.3f ms/put)\n",
               wmin / 1e3, wmed / 1e3, wmax / 1e3, bw_med(wmed),
               expected_puts
                   ? (wmed / 1e3) / static_cast<double>(expected_puts)
                   : 0.0);
  std::fprintf(stderr,
               "[SUMMARY] flush : min=%.3f med=%.3f max=%.3f ms "
               "(median %.1f MiB/s, %.3f ms/put)\n",
               fmin / 1e3, fmed / 1e3, fmax / 1e3, bw_med(fmed),
               expected_puts
                   ? (fmed / 1e3) / static_cast<double>(expected_puts)
                   : 0.0);
  if (opts.do_read) {
    std::fprintf(stderr,
                 "[SUMMARY] read  : min=%.3f med=%.3f max=%.3f ms "
                 "(median %.1f MiB/s, %.3f ms/get)\n",
                 rmin / 1e3, rmed / 1e3, rmax / 1e3, bw_med(rmed),
                 expected_gets
                     ? (rmed / 1e3) / static_cast<double>(expected_gets)
                     : 0.0);
  }
  std::fprintf(stderr,
               "[SUMMARY] write+flush+read : %.1f MiB/s end-to-end "
               "(median)\n",
               bw_med(wmed + fmed + rmed));
  return 0;
}

#else

int main() { return 0; }

#endif  // !HSHM_IS_DEVICE_PASS

#else

int main() { return 0; }

#endif  // (HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM) && !HSHM_ENABLE_SYCL

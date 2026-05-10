/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

/**
 * GPU vector unit test (CUDA / ROCm).
 *
 * 1. Bring up the Chimaera server + CTE core pool.
 * 2. Create a wrp_cte::gpu_vector::Vector<uint32_t> with 4 blocks,
 *    4 pages per block, 4 KiB pages.
 * 3. Launch a write kernel that does v[i] = i*2 over a striped pattern
 *    that crosses page boundaries.
 * 4. FlushAllSync().
 * 5. Launch a read kernel that copies v[i] into a result array, then
 *    verify on the host.
 */

#if (HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM) && !HSHM_ENABLE_SYCL

#include "simple_test.h"

#include <chimaera/bdev/bdev_client.h>
#include <chimaera/chimaera.h>
#include <chimaera/singletons.h>
#include <wrp_cte/core/core_client.h>
#include <wrp_cte/core/core_tasks.h>
#include <wrp_cte/gpu_vector/gpu_vector.h>

#include <hermes_shm/util/gpu_api.h>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
using TaskT = chimaera::admin::CreateTask;

namespace {

bool g_initialized = false;

/** Bring up Chimaera + CTE core pool exactly once. Body gated to host
 *  pass: the device pass parses the function but never runs it, and
 *  cte_client->AsyncCreate is HSHM_IS_HOST-only. */
void EnsureInit() {
#if !HSHM_IS_DEVICE_PASS
  if (g_initialized) return;
  std::fprintf(stderr, "[INIT] Starting Chimaera server (gpu_vector test)\n");
  REQUIRE(chi::CHIMAERA_INIT(chi::ChimaeraMode::kServer));
  REQUIRE(wrp_cte::core::WRP_CTE_CLIENT_INIT());
  auto *cte_client = WRP_CTE_CLIENT;
  REQUIRE(cte_client != nullptr);
  cte_client->Init(wrp_cte::core::kCtePoolId);
  wrp_cte::core::CreateParams params;
  auto create_task = cte_client->AsyncCreate(
      chi::PoolQuery::Dynamic(), wrp_cte::core::kCtePoolName,
      wrp_cte::core::kCtePoolId, params);
  create_task.Wait();
  REQUIRE(create_task->GetReturnCode() == 0);
  std::this_thread::sleep_for(50ms);

  // Register a kRam bdev target so PutBlob/GetBlob have somewhere to
  // land. Required for eviction tests since dirty pages are flushed
  // through the bdev runtime.
  const chi::u64 kRamCapacity = 4ULL << 30;  // 4 GiB
  chi::PoolId bdev_pool_id(950, 0);
  chimaera::bdev::Client bdev_client(bdev_pool_id);
  auto bdev_create = bdev_client.AsyncCreate(
      chi::PoolQuery::Dynamic(), std::string("gpu_vector_ram"),
      bdev_pool_id, chimaera::bdev::BdevType::kRam, kRamCapacity);
  bdev_create.Wait();
  REQUIRE(bdev_create->GetReturnCode() == 0);
  std::this_thread::sleep_for(50ms);
  auto reg_task = cte_client->AsyncRegisterTarget(
      "gpu_vector_ram", chimaera::bdev::BdevType::kRam, kRamCapacity,
      chi::PoolQuery::Local(), bdev_pool_id);
  reg_task.Wait();
  REQUIRE(reg_task->GetReturnCode() == 0);
  std::this_thread::sleep_for(50ms);

  g_initialized = true;
#endif
}

}  // namespace

namespace gv = wrp_cte::gpu_vector;
namespace dev = cte::gpu::dev;

/** Write v[i] = i*2 for the first total elements. */
__global__ void GpuVectorWriteKernel(chi::IpcManagerGpuInfo info,
                                      gv::DeviceView<chi::u32> view,
                                      chi::u64 total) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  // dev::vector ctor sets up the per-warp last-page __shared__ cache.
  dev::vector<chi::u32> v(view, g_ipc_manager_ptr);
  if (threadIdx.x != 0) return;
  chi::u64 stripe = view.page_capacity_t * view.base.pages_per_block;
  chi::u64 lo = blockIdx.x * stripe;
  chi::u64 hi = lo + stripe;
  if (hi > total) hi = total;
  for (chi::u64 i = lo; i < hi; ++i) {
    v[i] = static_cast<chi::u32>(i * 2u);
  }
  (void)g_ipc_manager;
}

/** Read v[i] back into result[i]. */
__global__ void GpuVectorReadKernel(chi::IpcManagerGpuInfo info,
                                     gv::DeviceView<chi::u32> view,
                                     chi::u32 *result, chi::u64 total) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  dev::vector<chi::u32> v(view, g_ipc_manager_ptr);
  if (threadIdx.x != 0) return;
  chi::u64 stripe = view.page_capacity_t * view.base.pages_per_block;
  chi::u64 lo = blockIdx.x * stripe;
  chi::u64 hi = lo + stripe;
  if (hi > total) hi = total;
  for (chi::u64 i = lo; i < hi; ++i) {
    // Implicit ElementRef -> chi::u32 conversion triggers a read.
    result[i] = v[i];
  }
  (void)g_ipc_manager;
}

/** Stress write: each block writes a contiguous stripe of `per_block`
 *  elements that spans many more pages than the cache holds, forcing
 *  per-page eviction + flush on every page miss. */
__global__ void GpuVectorWriteStressKernel(chi::IpcManagerGpuInfo info,
                                            gv::DeviceView<chi::u32> view,
                                            chi::u64 per_block) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  dev::vector<chi::u32> v(view, g_ipc_manager_ptr);
  if (threadIdx.x != 0) return;
  chi::u64 lo = static_cast<chi::u64>(blockIdx.x) * per_block;
  chi::u64 hi = lo + per_block;
  for (chi::u64 i = lo; i < hi; ++i) {
    v[i] = static_cast<chi::u32>(i ^ 0xC0FFEEUL);
  }
  (void)g_ipc_manager;
}

/** Stress verify: each block reads its stripe back and atomic-adds a
 *  counter for every mismatch. Sequential reads across pages much
 *  larger than the cache force a fresh fault per page. */
__global__ void GpuVectorVerifyStressKernel(chi::IpcManagerGpuInfo info,
                                             gv::DeviceView<chi::u32> view,
                                             chi::u64 per_block,
                                             chi::u32 *mismatch,
                                             chi::u64 *first_bad_idx) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  dev::vector<chi::u32> v(view, g_ipc_manager_ptr);
  if (threadIdx.x != 0) return;
  chi::u64 lo = static_cast<chi::u64>(blockIdx.x) * per_block;
  chi::u64 hi = lo + per_block;
  chi::u32 local_bad = 0;
  for (chi::u64 i = lo; i < hi; ++i) {
    chi::u32 expected = static_cast<chi::u32>(i ^ 0xC0FFEEUL);
    chi::u32 actual = v[i];
    if (actual != expected) {
      ++local_bad;
      atomicMin(reinterpret_cast<unsigned long long *>(first_bad_idx),
                static_cast<unsigned long long>(i));
    }
  }
  if (local_bad) atomicAdd(mismatch, local_bad);
  (void)g_ipc_manager;
}

#if !HSHM_IS_DEVICE_PASS

TEST_CASE("gpu_vector: write then read round-trip",
          "[gpu_vector][cte][stress]") {
  EnsureInit();
  auto *ipc = CHI_CPU_IPC;
  const chi::u32 nblocks = 4;
  const chi::u32 pages_per_block = 4;
  const chi::u64 page_size_bytes = 4096;

  // cache_period_ms=20 exercises the periodic CacheMgmtKernel.
  gv::Vector<chi::u32> vec("gpu_vector_smoke", nblocks, /*gpu_id=*/0,
                            pages_per_block, page_size_bytes,
                            /*cache_period_ms=*/20);

  chi::u64 elements_per_page = page_size_bytes / sizeof(chi::u32);
  chi::u64 total = static_cast<chi::u64>(nblocks) * pages_per_block *
                    elements_per_page;
  std::fprintf(stderr, "[GPUVEC] total=%llu elements\n",
               (unsigned long long)total);

  auto view = vec.Device();
  chi::IpcManagerGpuInfo gpu_info = ipc->GetGpuIpcManager()->GetGpuInfo(0);

  // hshm::GpuApi::MallocHost takes BYTES (not elements).
  auto *result = hshm::GpuApi::MallocHost<chi::u32>(
      total * sizeof(chi::u32));
  REQUIRE(result != nullptr);
  std::memset(result, 0, total * sizeof(chi::u32));

  // 1. Write v[i] = i*2 across all blocks.
  GpuVectorWriteKernel<<<nblocks, 32>>>(gpu_info, view, total);
  hshm::GpuApi::Synchronize();

  // 2. Drain in-flight puts before tearing down the cache.
  vec.FlushAllSync();

  // 3. Read back. Reads will fault any pages that the cache evicted.
  GpuVectorReadKernel<<<nblocks, 32>>>(gpu_info, view, result, total);
  hshm::GpuApi::Synchronize();

  // 4. Verify.
  for (chi::u64 i = 0; i < total; ++i) {
    if (result[i] != static_cast<chi::u32>(i * 2u)) {
      std::fprintf(stderr,
                   "[GPUVEC] mismatch at %llu: got %u expected %u\n",
                   (unsigned long long)i, result[i],
                   static_cast<chi::u32>(i * 2u));
      REQUIRE(result[i] == static_cast<chi::u32>(i * 2u));
    }
  }
  std::fprintf(stderr, "[GPUVEC] OK %llu / %llu\n",
               (unsigned long long)total, (unsigned long long)total);

  hshm::GpuApi::FreeHost(result);
}

SIMPLE_TEST_MAIN()

#endif  // !HSHM_IS_DEVICE_PASS

#else

int main() { return 0; }

#endif  // (HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM) && !HSHM_ENABLE_SYCL

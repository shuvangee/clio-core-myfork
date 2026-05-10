/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

#ifndef WRP_CTE_GPU_VECTOR_H_
#define WRP_CTE_GPU_VECTOR_H_

#include <chimaera/chimaera.h>
#include <chimaera/gpu/gpu_ipc_manager.h>
#include <chimaera/ipc_manager.h>
#include <chimaera/singletons.h>
#include <chimaera/types.h>
#include <hermes_shm/util/gpu_api.h>
#include <wrp_cte/core/core_client.h>
#include <wrp_cte/core/core_tasks.h>
#include <wrp_cte/gpu_vector/gpu_vector_kernels.h>
#include <wrp_cte/gpu_vector/gpu_vector_page.h>
#include <wrp_cte/gpu_vector/gpu_vector_view.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

#if HSHM_IS_GPU_COMPILER

namespace wrp_cte::gpu_vector {

/**
 * Producer-only GPU-resident vector backed by CTE blob storage. See
 * the file-level comment in gpu_vector_page.h for the data layout and
 * AGENTS.md "GPU Producer-Only Model" for the CHI_IPC->Send model.
 *
 * Lifecycle:
 *   - Caller must have already initialized the Chimaera runtime and
 *     the CTE core pool (see wrp_cte::core::WRP_CTE_CLIENT_INIT plus
 *     AsyncCreate(...)).
 *   - ctor: allocates the three backends (pages [kDeviceMem],
 *           meta [kDeviceMem], task pools [kPinnedHost]); registers
 *           them with the runtime; resolves/creates the tag;
 *           placement-news every PutBlob/GetBlob slot; runs an
 *           InitMeta kernel; spawns the cache-management thread.
 *   - dtor: stops the cache thread, FlushAllSync (CacheMgmtKernel +
 *           DrainKernel), unregisters + frees backends.
 */
template <typename T>
class Vector {
 public:
  Vector(const std::string &tag_name, chi::u32 nblocks,
         chi::u32 gpu_id = 0,
         chi::u32 pages_per_block = 4,
         chi::u64 page_size_bytes = 1ULL << 20,
         chi::u32 cache_period_ms = 50);
  ~Vector();

  Vector(const Vector &) = delete;
  Vector &operator=(const Vector &) = delete;

  /** Synchronously drain every dirty page. Launches the cache-mgmt
   *  kernel once and a drain kernel that calls Wait() on all in-flight
   *  put / get futures. */
  void FlushAllSync();

  /** POD captured by user kernels — pass by value into a __global__. */
  DeviceView<T> Device() const { return view_; }

  const wrp_cte::core::TagId &TagId() const { return view_.base.tag_id; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  DeviceView<T> view_{};
};

namespace detail {

/** One-shot init kernel — non-template so nvcc reliably registers it
 *  through the standard <<<>>> launch glue. Operates on DeviceViewBase
 *  so it doesn't need T. */
__global__ void InitMetaKernel(DeviceViewBase v, char *pages_base) {
  chi::u32 idx = blockIdx.x * blockDim.x + threadIdx.x;
  chi::u32 total = v.nblocks * v.pages_per_block;
  if (idx >= total) return;
  chi::u32 b_idx = idx / v.pages_per_block;
  chi::u32 slot = idx - b_idx * v.pages_per_block;
  Block *b = GetBlock(v, b_idx);
  if (slot == 0) {
    b->block_idx = b_idx;
    b->num_modified = 0;
    b->pages_per_block = v.pages_per_block;
  }
  Page *p = &b->pages[slot];
  p->device_ptr = pages_base +
      (static_cast<chi::u64>(b_idx) * v.pages_per_block + slot) *
       v.page_size_bytes;
  p->page_idx = -1;
  p->modify_min = -1;
  p->modify_max = -1;
  p->flags = 0;
  p->lru_clock = 0;
  // Default-construct the futures.
  new (&p->active_put) chi::gpu::Future<wrp_cte::core::PutBlobTask>();
  new (&p->active_get) chi::gpu::Future<wrp_cte::core::GetBlobTask>();
}

/** Cache-management kernel — non-template. Walks all (block, page)
 *  pairs and submits PutBlob for any with a non-empty modify range.
 *  Atomic exchange of [modify_min, modify_max] to -1 is done in
 *  FlushPage so the user kernel and this one don't race on the range. */
__global__ void CacheMgmtKernel(::chi::IpcManagerGpuInfo info,
                                 DeviceViewBase v) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  chi::u32 idx = blockIdx.x * blockDim.x + threadIdx.x;
  chi::u32 total = v.nblocks * v.pages_per_block;
  if (idx >= total) return;
  chi::u32 b_idx = idx / v.pages_per_block;
  chi::u32 slot = idx - b_idx * v.pages_per_block;
  Block *b = GetBlock(v, b_idx);
  Page *p = &b->pages[slot];
  if (p->modify_min < 0) return;
  // CAS the put-in-flight bit. Loser bails.
  if (detail::AtomicOrU32(&p->flags, kPagePutInFlight) & kPagePutInFlight) {
    return;
  }
  FlushPageBase(g_ipc_manager_ptr, v, b_idx, p, slot);
  (void)g_ipc_manager;
}

/** Drain kernel — non-template. Waits on every page's active_put /
 *  active_get and clears the slots. */
__global__ void DrainKernel(::chi::IpcManagerGpuInfo info, DeviceViewBase v) {
  CHIMAERA_GPU_INIT(info, /*ipc_ptr=*/nullptr);
  chi::u32 idx = blockIdx.x * blockDim.x + threadIdx.x;
  chi::u32 total = v.nblocks * v.pages_per_block;
  if (idx >= total) return;
  chi::u32 b_idx = idx / v.pages_per_block;
  chi::u32 slot = idx - b_idx * v.pages_per_block;
  Page *p = &GetBlock(v, b_idx)->pages[slot];
  DrainPut(p);
  DrainGet(p);
  (void)g_ipc_manager;
}

/** Compute the byte stride between adjacent Block structs in the meta
 *  backend, accounting for the flexible Page array. Aligned to 16. */
inline chi::u32 BlockStrideBytes(chi::u32 pages_per_block) {
  size_t raw = sizeof(Block) + sizeof(Page) * pages_per_block;
  return static_cast<chi::u32>((raw + 15) & ~static_cast<size_t>(15));
}

/** Sum of sizeof(TaskT) + sizeof(gpu::FutureShm), 16-byte aligned. */
template <typename TaskT>
inline chi::u32 TaskSlotStride() {
  size_t raw = sizeof(TaskT) + sizeof(chi::gpu::FutureShm);
  return static_cast<chi::u32>((raw + 15) & ~static_cast<size_t>(15));
}

}  // namespace detail

template <typename T>
struct Vector<T>::Impl {
  chi::u32 gpu_id = 0;
  chi::u32 cache_period_ms = 50;
  hipc::AllocatorId pages_alloc_id;
  hipc::AllocatorId meta_alloc_id;
  hipc::AllocatorId put_alloc_id;
  hipc::AllocatorId get_alloc_id;
  char *pages_base = nullptr;
  char *meta_base = nullptr;
  char *put_base = nullptr;
  char *get_base = nullptr;
  std::string tag_name;
  chi::PoolId cte_pool_id = chi::PoolId(0, 0);
  std::thread cache_thread;
  std::atomic<bool> cache_thread_run{false};
};

template <typename T>
inline Vector<T>::Vector(const std::string &tag_name, chi::u32 nblocks,
                          chi::u32 gpu_id, chi::u32 pages_per_block,
                          chi::u64 page_size_bytes,
                          chi::u32 cache_period_ms) {
#if !HSHM_IS_DEVICE_PASS
  // Body gated for the host pass only. The device pass sees an empty
  // ctor — it never instantiates Vector<T> on device anyway, but
  // nvcc / hipcc parse member-function bodies in both passes and the
  // host-only APIs we use here (Async*, GetGpuInfo, std::thread, ...)
  // are HSHM_IS_HOST-gated.
  if (nblocks == 0 || pages_per_block == 0 || page_size_bytes == 0) {
    throw std::invalid_argument(
        "wrp_cte::gpu_vector::Vector: nblocks/pages_per_block/page_size "
        "must all be > 0");
  }
  if (page_size_bytes % sizeof(T) != 0) {
    throw std::invalid_argument(
        "wrp_cte::gpu_vector::Vector: page_size_bytes must be a multiple "
        "of sizeof(T)");
  }
  impl_ = std::make_unique<Impl>();
  impl_->cache_period_ms = cache_period_ms;
  impl_->gpu_id = gpu_id;

  auto *cpu_ipc = CHI_CPU_IPC;

  // 1. Allocate the three backends.
  chi::u64 pages_bytes = static_cast<chi::u64>(nblocks) * pages_per_block *
                         page_size_bytes;
  impl_->pages_alloc_id = cpu_ipc->AllocateAndRegisterGpuBackend(
      gpu_id, chi::gpu::IpcManager::MemKind::kDeviceMem, pages_bytes,
      &impl_->pages_base);
  if (impl_->pages_alloc_id.IsNull()) {
    throw std::runtime_error("gpu_vector: pages_backend allocation failed");
  }

  chi::u32 block_stride = detail::BlockStrideBytes(pages_per_block);
  chi::u64 meta_bytes = static_cast<chi::u64>(block_stride) * nblocks;
  impl_->meta_alloc_id = cpu_ipc->AllocateAndRegisterGpuBackend(
      gpu_id, chi::gpu::IpcManager::MemKind::kDeviceMem, meta_bytes,
      &impl_->meta_base);
  if (impl_->meta_alloc_id.IsNull()) {
    throw std::runtime_error("gpu_vector: meta_backend allocation failed");
  }

  chi::u32 put_stride =
      detail::TaskSlotStride<wrp_cte::core::PutBlobTask>();
  chi::u32 get_stride =
      detail::TaskSlotStride<wrp_cte::core::GetBlobTask>();
  chi::u64 task_count = static_cast<chi::u64>(nblocks) * pages_per_block;
  impl_->put_alloc_id = cpu_ipc->AllocateAndRegisterGpuBackend(
      gpu_id, chi::gpu::IpcManager::MemKind::kPinnedHost,
      task_count * put_stride, &impl_->put_base);
  if (impl_->put_alloc_id.IsNull()) {
    throw std::runtime_error("gpu_vector: put_pool allocation failed");
  }
  impl_->get_alloc_id = cpu_ipc->AllocateAndRegisterGpuBackend(
      gpu_id, chi::gpu::IpcManager::MemKind::kPinnedHost,
      task_count * get_stride, &impl_->get_base);
  if (impl_->get_alloc_id.IsNull()) {
    throw std::runtime_error("gpu_vector: get_pool allocation failed");
  }
  std::memset(impl_->put_base, 0, task_count * put_stride);
  std::memset(impl_->get_base, 0, task_count * get_stride);

  // 2. Create the CTE tag. The caller is responsible for having
  //    initialized the CTE core pool (see test_core_client_config for
  //    the canonical pattern: WRP_CTE_CLIENT_INIT + AsyncCreate).
  wrp_cte::core::Client cte_client(wrp_cte::core::kCtePoolId);
  auto tag_fut = cte_client.AsyncGetOrCreateTag(tag_name);
  tag_fut.Wait();
  if (tag_fut->GetReturnCode() != 0) {
    throw std::runtime_error(
        "gpu_vector: GetOrCreateTag failed for tag '" + tag_name + "'");
  }
  view_.base.tag_id = tag_fut->tag_id_;
  impl_->tag_name = tag_name;
  impl_->cte_pool_id = wrp_cte::core::kCtePoolId;

  // 3. Populate DeviceView.
  view_.base.blocks = reinterpret_cast<Block *>(impl_->meta_base);
  view_.base.block_stride_bytes = block_stride;
  view_.base.pages_base = impl_->pages_base;
  view_.base.put_pool_base = impl_->put_base;
  view_.base.get_pool_base = impl_->get_base;
  view_.base.put_slot_stride = put_stride;
  view_.base.get_slot_stride = get_stride;
  view_.base.pages_alloc_id = impl_->pages_alloc_id;
  view_.base.put_pool_alloc_id = impl_->put_alloc_id;
  view_.base.get_pool_alloc_id = impl_->get_alloc_id;
  view_.base.nblocks = nblocks;
  view_.base.pages_per_block = pages_per_block;
  view_.base.page_size_bytes = page_size_bytes;
  view_.page_capacity_t = page_size_bytes / sizeof(T);

  // 4. Placement-new every PutBlob/GetBlob slot with stable fields
  //    (tag_id, blob_name, pool_query, pool_id, method). The kernel
  //    only mutates offset_/size_/blob_data_ on each Send.
  for (chi::u32 b = 0; b < nblocks; ++b) {
    for (chi::u32 p = 0; p < pages_per_block; ++p) {
      chi::u64 slot_idx = static_cast<chi::u64>(b) * pages_per_block + p;
      char *put_addr = impl_->put_base + slot_idx * put_stride;
      char *get_addr = impl_->get_base + slot_idx * get_stride;
      // Blob name format: "<tag>_b<block>_p<slot>"
      std::string blob_name = tag_name + "_b" + std::to_string(b) +
                              "_p" + std::to_string(p);
      auto put_task = new (put_addr) wrp_cte::core::PutBlobTask(
          chi::CreateTaskId(), impl_->cte_pool_id,
          chi::PoolQuery::ToLocalCpu(), view_.base.tag_id,
          blob_name.c_str(), /*offset=*/0, /*size=*/0,
          hipc::ShmPtr<>::GetNull(), /*score=*/-1.0f,
          wrp_cte::core::Context(), /*flags=*/0);
      put_task->pod_size_ = static_cast<chi::u32>(sizeof(*put_task));
      new (put_addr + sizeof(*put_task)) chi::gpu::FutureShm();

      auto get_task = new (get_addr) wrp_cte::core::GetBlobTask(
          chi::CreateTaskId(), impl_->cte_pool_id,
          chi::PoolQuery::ToLocalCpu(), view_.base.tag_id,
          blob_name.c_str(), /*offset=*/0, /*size=*/0,
          /*flags=*/0, hipc::ShmPtr<>::GetNull());
      get_task->pod_size_ = static_cast<chi::u32>(sizeof(*get_task));
      new (get_addr + sizeof(*get_task)) chi::gpu::FutureShm();
    }
  }

  // 5. Initialize the meta backend on-device.
  chi::u32 init_total = nblocks * pages_per_block;
  chi::u32 init_threads = init_total < 256u ? init_total : 256u;
  chi::u32 init_blocks = (init_total + init_threads - 1) / init_threads;
  detail::InitMetaKernel<<<init_blocks, init_threads>>>(
      view_.base, static_cast<char *>(impl_->pages_base));
  hshm::GpuApi::Synchronize();

  // 6. Spawn the cache-management thread (skip if cache_period_ms == 0).
  if (cache_period_ms == 0) return;
  impl_->cache_thread_run.store(true);
  Vector<T> *self = this;
  impl_->cache_thread = std::thread([self]() {
    auto *gpu_ipc_mgr = CHI_CPU_IPC->GetGpuIpcManager();
    chi::IpcManagerGpuInfo info = gpu_ipc_mgr->GetGpuInfo(self->impl_->gpu_id);
    while (self->impl_->cache_thread_run.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(self->impl_->cache_period_ms));
      if (!self->impl_->cache_thread_run.load()) break;
      chi::u32 total =
          self->view_.base.nblocks * self->view_.base.pages_per_block;
      // <<<total, 1>>>: one thread per (block,slot) pair, each as
      // threadIdx.x==0 so ClientSend's producer constraint is satisfied.
      detail::CacheMgmtKernel<<<total, 1>>>(info, self->view_.base);
      hshm::GpuApi::Synchronize();
    }
  });
#else
  (void)tag_name; (void)nblocks; (void)gpu_id;
  (void)pages_per_block; (void)page_size_bytes; (void)cache_period_ms;
#endif  // !HSHM_IS_DEVICE_PASS
}

template <typename T>
inline Vector<T>::~Vector() {
#if !HSHM_IS_DEVICE_PASS
  if (!impl_) return;
  // Stop the cache thread first so it doesn't race the drain kernel.
  impl_->cache_thread_run.store(false, std::memory_order_release);
  if (impl_->cache_thread.joinable()) impl_->cache_thread.join();
  try {
    FlushAllSync();
  } catch (...) {
    // best-effort drain; the dtor must not throw
  }
  auto *cpu_ipc = CHI_CPU_IPC;
  if (impl_->pages_base)
    cpu_ipc->FreeGpuBackend(impl_->gpu_id, impl_->pages_alloc_id);
  if (impl_->meta_base)
    cpu_ipc->FreeGpuBackend(impl_->gpu_id, impl_->meta_alloc_id);
  if (impl_->put_base)
    cpu_ipc->FreeGpuBackend(impl_->gpu_id, impl_->put_alloc_id);
  if (impl_->get_base)
    cpu_ipc->FreeGpuBackend(impl_->gpu_id, impl_->get_alloc_id);
#endif  // !HSHM_IS_DEVICE_PASS
}

template <typename T>
inline void Vector<T>::FlushAllSync() {
#if !HSHM_IS_DEVICE_PASS
  auto *gpu_ipc_mgr = CHI_CPU_IPC->GetGpuIpcManager();
  chi::IpcManagerGpuInfo info = gpu_ipc_mgr->GetGpuInfo(impl_->gpu_id);
  // Launch one thread per (block,slot) pair, each in its own GPU block,
  // so every thread has threadIdx.x == 0 — required by
  // IpcGpu2Cpu::ClientSend (only thread 0 of a block actually pushes
  // onto the gpu2cpu queue; non-zero threads return an empty future
  // but still atomically reset the page's modify_min, orphaning the
  // dirty data). Same constraint applies to DrainKernel since it
  // can race with FlushPage submissions.
  chi::u32 total = view_.base.nblocks * view_.base.pages_per_block;
  detail::CacheMgmtKernel<<<total, 1>>>(info, view_.base);
  hshm::GpuApi::Synchronize();
  detail::DrainKernel<<<total, 1>>>(info, view_.base);
  hshm::GpuApi::Synchronize();
#endif
}

}  // namespace wrp_cte::gpu_vector

#endif  // HSHM_IS_GPU_COMPILER

#endif  // WRP_CTE_GPU_VECTOR_H_

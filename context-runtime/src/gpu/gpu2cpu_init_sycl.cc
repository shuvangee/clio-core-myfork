/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * SYCL host-side GPU2CPU queue + backend initialization.
 *
 * Under the SYCL backend the GPU is a pure task producer: kernels push
 * tasks onto a pinned-host gpu2cpu_queue via IpcGpu2Cpu::ClientSend, and
 * the CPU GPU worker (Worker::ProcessNewTaskGpu) pops them. The CUDA
 * orchestrator infrastructure (cpu2gpu / gpu2gpu / internal queues, the
 * persistent device-side worker kernel, per-chimod GpuRuntime
 * containers) does not exist on this backend.
 *
 * This file allocates two pinned-host SYCL USM regions per logical GPU:
 *   - gpu2cpu_queue_backend: holds the GpuTaskQueue (BuddyAllocator +
 *     multi_mpsc_ring_buffer<gpu::Future<Task>>), constructed in place.
 *   - gpu2cpu_copy_backend:  holds the GPU client's RoundRobinAllocator
 *     and partition pool. Inside a SYCL kernel,
 *     CHIMAERA_GPU_INIT(gpu_info, ipc_ptr) calls
 *     gpu::IpcManager::ClientInitGpu which placement-news a
 *     RoundRobinAllocator at gpu_info.backend.data_; subsequent
 *     CHI_IPC->NewTask<TaskT>(...) calls allocate Task+FutureShm pairs
 *     from this backend.
 *
 * The queue and allocator are constructed entirely on the host (no SYCL
 * kernel needed) because pinned-host USM is bidirectionally accessible
 * via plain pointer reads/writes — the device sees the same byte layout
 * the host wrote.
 */

#if HSHM_ENABLE_SYCL && !(HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM)

#include "chimaera/ipc_manager.h"
#include "chimaera/gpu/gpu_ipc_manager.h"
#include "chimaera/config_manager.h"
#include "chimaera/singletons.h"
#include "hermes_shm/util/gpu_api.h"
#include "hermes_shm/util/logging.h"

#include <sycl/sycl.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace chi {

//==============================================================================
// gpu::IpcManager::ServerInitGpuQueuesSycl
//==============================================================================

/**
 * Allocate the gpu2cpu_queue and gpu2cpu_backend in pinned-host SYCL USM
 * and populate gpu_orchestrator_info_ so subsequent GetGpuInfo calls see
 * valid pointers.
 *
 * Memory layout (one logical device on SYCL):
 *   sycl_gpu_devices_[0].gpu2cpu_queue_backend (sycl::malloc_host, 16 MB)
 *     +-- BuddyAllocator metadata (placed by MakeAlloc)
 *     +-- chi::GpuTaskQueue object (allocated via NewObj)
 *   sycl_gpu_devices_[0].gpu2cpu_copy_backend  (sycl::malloc_host, 64 MB)
 *     |  contents are populated lazily inside the kernel by
 *     |  CHIMAERA_GPU_INIT -> ClientInitGpu -> InitHeapAllocator
 *
 * @param queue_depth Ring-buffer depth per lane.
 * @param backend_bytes Size of the GPU client allocation backend.
 * @return true on success; false if any USM allocation or queue
 *         construction step fails.
 */
namespace {
/** Opaque kernel-name type for the SYCL queue-construction kernel. */
class chimaera_sycl_init_queue_kernel;
}

bool gpu::IpcManager::ServerInitGpuQueuesSycl(u32 queue_depth,
                                              size_t backend_bytes) {
  // One logical device on SYCL — the backend is a single shared SYCL
  // context, so we don't enumerate per-physical-GPU like CUDA does.
  if (!sycl_gpu_devices_.empty()) {
    return true;  // already initialized
  }
  sycl_gpu_devices_.resize(1);
  SyclGpuDeviceInfo &dev = sycl_gpu_devices_[0];

  // ---- 1. gpu2cpu_queue_backend (pinned-host USM holding the queue) ----
  constexpr size_t kQueueBackendBytes = 16 * 1024 * 1024;
  auto &q = hshm::GpuApi::SyclQueue();
  dev.gpu2cpu_queue_backend = static_cast<char *>(
      sycl::malloc_host(kQueueBackendBytes, q));
  if (!dev.gpu2cpu_queue_backend) {
    HLOG(kError, "ServerInitGpuQueuesSycl: malloc_host({}MB) for "
         "gpu2cpu_queue_backend failed",
         kQueueBackendBytes / (1024 * 1024));
    return false;
  }
  dev.gpu2cpu_queue_backend_size = kQueueBackendBytes;
  std::memset(dev.gpu2cpu_queue_backend, 0, kQueueBackendBytes);

  // Construct the BuddyAllocator + GpuTaskQueue inside a SYCL device-side
  // kernel. The allocator metadata stores intra-region pointers; running
  // the construction on the device ensures the resulting pointer values
  // are valid in device address space. Without this, an SM dereferencing
  // host-constructed allocator state hits CUDA_ERROR_ILLEGAL_ADDRESS.
  // The output offset lives in shared USM so the host can read it back.
  size_t *out_off = sycl::malloc_shared<size_t>(1, q);
  if (!out_off) {
    HLOG(kError, "ServerInitGpuQueuesSycl: malloc_shared(out_off) failed");
    sycl::free(dev.gpu2cpu_queue_backend, q);
    dev.gpu2cpu_queue_backend = nullptr;
    return false;
  }
  *out_off = static_cast<size_t>(-1);

  char *queue_backend_ptr = dev.gpu2cpu_queue_backend;
  size_t queue_backend_size = kQueueBackendBytes;
  q.submit([&](sycl::handler &cgh) {
    cgh.single_task<chimaera_sycl_init_queue_kernel>([=]() {
      hipc::MemoryBackend proxy;
      proxy.data_ = queue_backend_ptr;
      proxy.data_capacity_ = queue_backend_size;
      CHI_QUEUE_ALLOC_T *alloc = proxy.MakeAlloc<CHI_QUEUE_ALLOC_T>();
      if (!alloc) {
        *out_off = static_cast<size_t>(-1);
        return;
      }
      hipc::FullPtr<chi::GpuTaskQueue> queue = alloc->NewObj<chi::GpuTaskQueue>(
          alloc, /*num_lanes=*/1u, /*num_prio=*/2u, queue_depth);
      *out_off = queue.IsNull() ? static_cast<size_t>(-1)
                                : queue.shm_.off_.load();
    });
  }).wait_and_throw();

  size_t queue_off = *out_off;
  sycl::free(out_off, q);
  if (queue_off == static_cast<size_t>(-1)) {
    HLOG(kError, "ServerInitGpuQueuesSycl: device-side queue construction "
         "failed");
    sycl::free(dev.gpu2cpu_queue_backend, q);
    dev.gpu2cpu_queue_backend = nullptr;
    return false;
  }
  dev.gpu2cpu_queue.shm_.off_ = queue_off;
  dev.gpu2cpu_queue.shm_.alloc_id_ = hipc::AllocatorId{0, 0};
  dev.gpu2cpu_queue.ptr_ = reinterpret_cast<chi::GpuTaskQueue *>(
      dev.gpu2cpu_queue_backend + queue_off);

  // ---- 2. gpu2cpu_copy_backend (pinned-host USM for client allocs) ----
  dev.gpu2cpu_copy_backend = static_cast<char *>(
      sycl::malloc_host(backend_bytes, q));
  if (!dev.gpu2cpu_copy_backend) {
    HLOG(kError, "ServerInitGpuQueuesSycl: malloc_host({}MB) for "
         "gpu2cpu_copy_backend failed",
         backend_bytes / (1024 * 1024));
    sycl::free(dev.gpu2cpu_queue_backend, q);
    dev.gpu2cpu_queue_backend = nullptr;
    dev.gpu2cpu_queue = hipc::FullPtr<chi::GpuTaskQueue>::GetNull();
    return false;
  }
  dev.gpu2cpu_copy_backend_size = backend_bytes;
  std::memset(dev.gpu2cpu_copy_backend, 0, backend_bytes);

  // ---- 3. Populate gpu_orchestrator_info_ for GetGpuInfo / Send paths ----
  gpu_orchestrator_info_.gpu2cpu_queue = dev.gpu2cpu_queue.ptr_;
  gpu_orchestrator_info_.backend.data_ = dev.gpu2cpu_copy_backend;
  gpu_orchestrator_info_.backend.data_capacity_ = dev.gpu2cpu_copy_backend_size;
  gpu_orchestrator_info_.backend.id_ = hipc::MemoryBackendId{4001, 0};
  gpu_orchestrator_info_.gpu2cpu_backend = gpu_orchestrator_info_.backend;
  gpu_orchestrator_info_.gpu_queue_depth = queue_depth;
  // SYCL has no cpu2gpu / gpu2gpu queues — leave those null.
  gpu_orchestrator_info_.cpu2gpu_queue = nullptr;
  gpu_orchestrator_info_.gpu2gpu_queue = nullptr;
  gpu_orchestrator_info_.internal_queue = nullptr;

  HLOG(kInfo,
       "ServerInitGpuQueuesSycl: gpu2cpu_queue at {} (queue_backend={}MB), "
       "gpu2cpu_backend at {} ({}MB)",
       static_cast<void *>(dev.gpu2cpu_queue.ptr_),
       kQueueBackendBytes / (1024 * 1024),
       static_cast<void *>(dev.gpu2cpu_copy_backend),
       backend_bytes / (1024 * 1024));
  return true;
}

//==============================================================================
// gpu::IpcManager::FinalizeGpuQueuesSycl
//==============================================================================

/**
 * Free all SYCL USM regions allocated by ServerInitGpuQueuesSycl. Safe
 * to call multiple times.
 */
void gpu::IpcManager::FinalizeGpuQueuesSycl() {
  if (sycl_gpu_devices_.empty()) return;
  auto &q = hshm::GpuApi::SyclQueue();
  for (auto &dev : sycl_gpu_devices_) {
    if (dev.gpu2cpu_copy_backend) {
      sycl::free(dev.gpu2cpu_copy_backend, q);
      dev.gpu2cpu_copy_backend = nullptr;
    }
    if (dev.gpu2cpu_queue_backend) {
      sycl::free(dev.gpu2cpu_queue_backend, q);
      dev.gpu2cpu_queue_backend = nullptr;
    }
    dev.gpu2cpu_queue = hipc::FullPtr<chi::GpuTaskQueue>::GetNull();
  }
  sycl_gpu_devices_.clear();
  gpu_orchestrator_info_.gpu2cpu_queue = nullptr;
  gpu_orchestrator_info_.backend.data_ = nullptr;
  gpu_orchestrator_info_.backend.data_capacity_ = 0;
}

//==============================================================================
// gpu::IpcManager::GetGpuInfo (SYCL)
//==============================================================================

/**
 * Return a copy of gpu_orchestrator_info_ (cpu2gpu/gpu2gpu queues stay
 * null on SYCL — only ToLocalCpu routing is supported). Mirrors the
 * CUDA/ROCm GetGpuInfo signature for call-site parity.
 */
IpcManagerGpuInfo gpu::IpcManager::GetGpuInfo(u32 gpu_id) const {
  if (sycl_gpu_devices_.empty() || gpu_id >= sycl_gpu_devices_.size()) {
    return IpcManagerGpuInfo{};
  }
  return gpu_orchestrator_info_;
}

//==============================================================================
// gpu::IpcManager::CreateGpuAllocator (SYCL)
//==============================================================================

/**
 * SYCL: a single shared backend serves every kernel. CreateGpuAllocator
 * is therefore a thin wrapper around GetGpuInfo — there is no per-call
 * device-memory allocation like the CUDA path's GpuMalloc(...).
 *
 * @param gpu_memory_size Ignored under SYCL (the backend is sized once
 *        at server-init time via ServerInitGpuQueuesSycl's backend_bytes
 *        argument).
 * @param gpu_id Logical GPU id (always 0 on SYCL).
 * @return Populated IpcManagerGpuInfo.
 */
IpcManagerGpuInfo gpu::IpcManager::CreateGpuAllocator(size_t gpu_memory_size,
                                                      u32 gpu_id) {
  (void)gpu_memory_size;
  return GetGpuInfo(gpu_id);
}

}  // namespace chi

//==============================================================================
// Host-callable bootstrap entry point
//==============================================================================
//
// Called from chi::IpcManager::ServerInit() in chimaera_cxx (which does NOT
// have HSHM_ENABLE_SYCL=1 set so it can't directly invoke gpu::IpcManager
// methods). The host-side TU forward-declares this function with C++ linkage
// and calls it unconditionally; the body lives here in chimaera_cxx_gpu and
// is the only place that needs to know about SYCL APIs.
//
// Returns true on success, false if any SYCL allocation failed. Populates
// *self->gpu_ipc_ via std::make_unique<gpu::IpcManager>() if needed and
// pushes the resulting gpu2cpu_queue into self->gpu_queues_ so
// AssignGpuLanesToWorker finds it.

namespace chi {

bool ChiServerBootstrapSyclGpu(IpcManager *self, chi::u32 queue_depth,
                                size_t backend_bytes) {
  if (!self) return false;
  if (!self->gpu_ipc_) {
    self->gpu_ipc_ = std::make_unique<gpu::IpcManager>();
  }
  if (!self->gpu_ipc_->ServerInitGpuQueuesSycl(queue_depth, backend_bytes)) {
    return false;
  }
  // Make AssignGpuLanesToWorker / GetGpuQueueCount see this queue via the
  // non-CUDA fallback path (gpu_queues_).
  self->RegisterGpuQueue(self->gpu_ipc_->sycl_gpu_devices_[0].gpu2cpu_queue);
  return true;
}

}  // namespace chi

#endif  // HSHM_ENABLE_SYCL && !(CUDA||ROCM)

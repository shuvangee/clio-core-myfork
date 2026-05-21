/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

/**
 * Slim CUDA / ROCm gpu2cpu init.
 *
 * Producer-only design: per detected GPU, allocate one pinned-host backend
 * holding a chi::GpuTaskQueue. The CPU GPU worker polls each queue;
 * kernels push admin-registered task allocations onto them via
 * IpcGpu2Cpu::ClientSend. There is no longer a per-device "copy_backend"
 * — clients allocate their own task and data backends on the host and
 * register them through admin RegisterMemory.
 */

#if (CTP_ENABLE_CUDA || CTP_ENABLE_ROCM) && !CTP_ENABLE_SYCL

#include "clio_runtime/ipc_manager.h"
#include "clio_runtime/gpu/gpu_ipc_manager.h"
#include "clio_runtime/config_manager.h"
#include "clio_runtime/device_memcpy.h"
#include "clio_runtime/singletons.h"
#include "clio_ctp/util/gpu_api.h"
#include "clio_ctp/util/logging.h"

#include <cstring>
#include <memory>
#include <new>

namespace clio::run {

// Note: queue construction happens host-side (see ServerInitGpuQueues).
// We previously launched a single-thread kernel to construct the
// BuddyAllocator + GpuTaskQueue in device memory, but the kernel had no
// host/device asymmetry that required device-side construction (the
// queue_backend is pinned host memory mapped 1:1 into device address
// space) and the cross-shared-library kernel registration was unreliable
// under HIP-NVCC ("cudaErrorInvalidDeviceFunction" on launch).

#if CTP_IS_HOST

bool gpu::IpcManager::ServerInitGpuQueues(u32 queue_depth) {
  if (!per_gpu_devices_.empty()) {
    return true;  // already initialized
  }

  int device_count = static_cast<int>(ctp::GpuApi::GetDeviceCount());
  if (device_count <= 0) {
    // CPU-only deployment: leave per_gpu_devices_ empty. Consumers
    // (GetGpuInfo, GetGpuQueue, RegisterClientBackend) all bounds-check
    // against per_gpu_devices_.size() and gracefully return null for
    // unknown gpu_ids, so a runtime without GPUs operates correctly —
    // it just never services GPU→CPU tasks. Returning true here lets
    // CHIMAERA_INIT complete on hosts and CI containers without a
    // visible CUDA device.
    HLOG(kInfo, "ServerInitGpuQueues: no GPU devices detected — "
         "GPU queues will not be initialized (CPU-only mode)");
    return true;
  }
  per_gpu_devices_.resize(device_count);

  constexpr size_t kQueueBackendBytes = 16 * 1024 * 1024;

  for (int gpu_id = 0; gpu_id < device_count; ++gpu_id) {
    PerGpuDeviceState &dev = per_gpu_devices_[gpu_id];
    dev.gpu_id = static_cast<u32>(gpu_id);

    ctp::GpuApi::SetDevice(gpu_id);

    dev.queue_backend = ctp::GpuApi::MallocHost<char>(kQueueBackendBytes);
    if (!dev.queue_backend) {
      HLOG(kError, "ServerInitGpuQueues: MallocHost for queue backend "
           "failed (gpu_id={})", gpu_id);
      FinalizeGpuQueues();
      return false;
    }
    dev.queue_backend_size = kQueueBackendBytes;
    std::memset(dev.queue_backend, 0, kQueueBackendBytes);

    // Host-side construction. queue_backend is pinned host memory mapped
    // into device address space at the same virtual address, so the
    // BuddyAllocator's internal offset-based bookkeeping is safe to set
    // up from the host. We previously constructed inside a single-thread
    // CUDA kernel; under HIP-NVCC that path hit "invalid device function"
    // intermittently, and the kernel had no host/device asymmetry that
    // required device-side construction in the first place.
    size_t queue_off = static_cast<size_t>(-1);
    {
      ctp::ipc::MemoryBackend proxy;
      proxy.data_ = dev.queue_backend;
      proxy.data_capacity_ = kQueueBackendBytes;
      CLIO_QUEUE_ALLOC_T *alloc = proxy.MakeAlloc<CLIO_QUEUE_ALLOC_T>();
      if (alloc) {
        ctp::ipc::FullPtr<chi::GpuTaskQueue> queue =
            alloc->NewObj<chi::GpuTaskQueue>(
                alloc, /*num_lanes=*/1u, /*num_prio=*/2u, queue_depth);
        if (!queue.IsNull()) {
          queue_off = queue.shm_.off_.load();
        }
      }
    }
    if (queue_off == static_cast<size_t>(-1)) {
      HLOG(kError, "ServerInitGpuQueues: queue construction failed "
           "(gpu_id={})", gpu_id);
      FinalizeGpuQueues();
      return false;
    }
    dev.gpu2cpu_queue.shm_.off_ = queue_off;
    dev.gpu2cpu_queue.shm_.alloc_id_ = ctp::ipc::AllocatorId{0, 0};
    dev.gpu2cpu_queue.ptr_ = reinterpret_cast<chi::GpuTaskQueue *>(
        dev.queue_backend + queue_off);

    HLOG(kInfo, "ServerInitGpuQueues: gpu_id={} queue at {} ({}MB)",
         gpu_id, static_cast<void *>(dev.gpu2cpu_queue.ptr_),
         kQueueBackendBytes / (1024 * 1024));
  }

  // Install device-aware memcpy + IsDevicePointer hooks (consumed by the
  // bdev runtime when WriteToRam / ReadFromRam see device USM pointers,
  // and by the GPU2CPU worker pop path when D2H/H2D-copying POD tasks).
  //
  // We MUST use a non-blocking, per-thread stream here. The kernel that
  // submitted the task is parked on a Wait() spin-loop on its own
  // (default) stream, polling FUTURE_COMPLETE on the device-side
  // gpu::FutureShm. If we issued cudaMemcpy on the legacy default
  // stream it would block until prior default-stream work (the spinning
  // kernel) drained — classic CUDA deadlock. cudaMemcpyAsync on a
  // separate non-blocking stream uses an independent DMA engine and
  // proceeds concurrently with the kernel.
  chi::g_device_aware_memcpy.store(
      [](void *dst, const void *src, std::size_t n) {
        if (n == 0) return;
#if CTP_ENABLE_CUDA
        // Fast path: when both pointers are plain host memory, use
        // std::memcpy. The CUDA host->host path goes through the driver's
        // generic copy and tops out at ~1.5-3 GB/s on x86; std::memcpy
        // hits 8-15 GB/s on a single core. Pinned host (cudaMemoryTypeHost)
        // and unregistered system memory both work with plain memcpy.
        // Managed (UVM) and Device pointers stay on the cudaMemcpyAsync
        // path so the driver handles migration / D2H / D2D correctly.
        auto is_host_kind = [](const void *p) {
          cudaPointerAttributes a{};
          cudaError_t rc = cudaPointerGetAttributes(&a, p);
          if (rc == cudaErrorInvalidValue) {
            // Older CUDA returned this for unregistered host memory; clear
            // the sticky error so it doesn't trip the next CUDA call.
            (void)cudaGetLastError();
            return true;
          }
          if (rc != cudaSuccess) return false;
          return a.type == cudaMemoryTypeHost ||
                 a.type == cudaMemoryTypeUnregistered;
        };
        if (is_host_kind(dst) && is_host_kind(src)) {
          std::memcpy(dst, src, n);
          return;
        }
        thread_local cudaStream_t s = nullptr;
        if (!s) {
          CUDA_ERROR_CHECK(cudaStreamCreateWithFlags(
              &s, cudaStreamNonBlocking));
        }
        CUDA_ERROR_CHECK(cudaMemcpyAsync(dst, src, n,
                                          cudaMemcpyDefault, s));
        CUDA_ERROR_CHECK(cudaStreamSynchronize(s));
#elif CTP_ENABLE_ROCM
        auto is_host_kind = [](const void *p) {
          hipPointerAttribute_t a{};
          hipError_t rc = hipPointerGetAttributes(&a, p);
          if (rc == hipErrorInvalidValue) {
            (void)hipGetLastError();
            return true;
          }
          if (rc != hipSuccess) return false;
#if HIP_VERSION >= 60000000
          return a.type == hipMemoryTypeHost ||
                 a.type == hipMemoryTypeUnregistered;
#else
          return a.memoryType == hipMemoryTypeHost ||
                 a.memoryType == hipMemoryTypeUnregistered;
#endif
        };
        if (is_host_kind(dst) && is_host_kind(src)) {
          std::memcpy(dst, src, n);
          return;
        }
        thread_local hipStream_t s = nullptr;
        if (!s) {
          HIP_ERROR_CHECK(hipStreamCreateWithFlags(
              &s, hipStreamNonBlocking));
        }
        HIP_ERROR_CHECK(hipMemcpyAsync(dst, src, n,
                                        hipMemcpyDefault, s));
        HIP_ERROR_CHECK(hipStreamSynchronize(s));
#else
        ctp::GpuApi::Memcpy(static_cast<char *>(dst),
                             static_cast<const char *>(src), n);
#endif
      },
      std::memory_order_release);
  chi::g_is_device_pointer.store(
      [](const void *ptr) -> bool {
        return ctp::GpuApi::IsDevicePointer(const_cast<void *>(ptr));
      },
      std::memory_order_release);

  return true;
}

void gpu::IpcManager::FinalizeGpuQueues() {
  for (auto &dev : per_gpu_devices_) {
    if (dev.queue_backend) {
      ctp::GpuApi::FreeHost(dev.queue_backend);
      dev.queue_backend = nullptr;
    }
    dev.gpu2cpu_queue = ctp::ipc::FullPtr<chi::GpuTaskQueue>::GetNull();
    dev.client_backends.clear();
  }
  per_gpu_devices_.clear();
}

bool gpu::IpcManager::RegisterClientBackend(const ClientBackend &b) {
  if (b.gpu_id >= per_gpu_devices_.size()) return false;
  u64 key = (static_cast<u64>(b.alloc_id.major_) << 32) |
            static_cast<u64>(b.alloc_id.minor_);
  auto &dev = per_gpu_devices_[b.gpu_id];
  dev.client_backends[key] = b;
  return true;
}

void gpu::IpcManager::UnregisterClientBackend(
    u32 gpu_id, const ctp::ipc::AllocatorId &alloc_id) {
  if (gpu_id >= per_gpu_devices_.size()) return;
  u64 key = (static_cast<u64>(alloc_id.major_) << 32) |
            static_cast<u64>(alloc_id.minor_);
  per_gpu_devices_[gpu_id].client_backends.erase(key);
}

// FindClientBackend is now inline in gpu_ipc_manager.h so it's
// available without linking libchimaera_cxx_gpu (used by ToFullPtr).

bool ChiServerBootstrapHipGpu(IpcManager *self, chi::u32 queue_depth,
                               std::size_t backend_bytes) {
  (void)backend_bytes;  // No host-managed copy_backend in producer-only model.
  if (!self) return false;
  if (!self->gpu_ipc_) {
    self->gpu_ipc_ = std::make_unique<gpu::IpcManager>();
  }
  return self->gpu_ipc_->ServerInitGpuQueues(queue_depth);
}

#endif  // CTP_IS_HOST

}  // namespace clio::run

#endif  // (CTP_ENABLE_CUDA || CTP_ENABLE_ROCM) && !CTP_ENABLE_SYCL

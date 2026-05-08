/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

#ifndef CHIMAERA_INCLUDE_CHIMAERA_IPC_GPU2CPU_IMPL_H_
#define CHIMAERA_INCLUDE_CHIMAERA_IPC_GPU2CPU_IMPL_H_

#include "chimaera/ipc/ipc_gpu2cpu.h"
#include "hermes_shm/util/gpu_intrinsics.h"

#if HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM || HSHM_ENABLE_SYCL

namespace chi {

#if HSHM_IS_GPU_COMPILER || HSHM_IS_SYCL_COMPILER
/**
 * GPU-side ClientSend.
 *
 * Producer-only design: the host pre-allocated Task+FutureShm in a
 * registered backend and passed `task_ptr` to the kernel. The kernel
 * already mutated POD input fields. We just clear the FutureShm flags,
 * build a gpu::Future<TaskT> carrying ShmPtrs to both the task and its
 * co-located FutureShm, then push onto gpu2cpu_queue with a system fence.
 *
 * Threading:
 *   - CUDA/ROCm: only thread 0 of the block enqueues; other threads
 *     return an empty future (caller is expected to broadcast).
 *   - SYCL: kernels are single_task by convention, so the full WI runs.
 */
template <typename TaskT>
HSHM_GPU_FUN gpu::Future<TaskT> IpcGpu2Cpu::ClientSend(
    gpu::IpcManager *ipc, const hipc::FullPtr<TaskT> &task_ptr) {
  gpu::Future<TaskT> future;

#if HSHM_IS_GPU_COMPILER
  if (threadIdx.x != 0) return future;
#endif

  if (task_ptr.IsNull() || !ipc->gpu_info_.gpu2cpu_queue) {
    return future;
  }

  // Co-located FutureShm sits immediately after the POD task struct.
  gpu::FutureShm *fshm = reinterpret_cast<gpu::FutureShm *>(
      reinterpret_cast<char *>(task_ptr.ptr_) + sizeof(TaskT));
  fshm->Reset(static_cast<u32>(sizeof(TaskT)));

  // For kPinnedHost / kManagedUvm backends the host_view pointer is also
  // the device-accessible address — the kernel and the CPU worker can both
  // dereference it directly. We therefore stash the *raw address* in off_
  // and keep the AllocatorId for backend lookup. The worker pop path
  // dereferences off_ directly without per-backend resolution math. When
  // kDeviceMem is wired up the worker will branch on `kind` and issue a
  // cudaMemcpy of size sizeof(FutureShm) from device_ptr+task_off instead.
  hipc::ShmPtr<gpu::FutureShm> fshmptr;
  fshmptr.alloc_id_ = task_ptr.shm_.alloc_id_;
  fshmptr.off_ = reinterpret_cast<size_t>(fshm);

  // Mirror raw addressing for the task ShmPtr in the queue entry so the
  // CPU worker can dereference the task pointer the same way.
  hipc::FullPtr<Task> task_for_queue;
  task_for_queue.shm_.alloc_id_ = task_ptr.shm_.alloc_id_;
  task_for_queue.shm_.off_ = reinterpret_cast<size_t>(task_ptr.ptr_);
  task_for_queue.ptr_ = static_cast<Task *>(task_ptr.ptr_);

  future = gpu::Future<TaskT>(fshmptr, task_ptr);

  gpu::Future<Task> task_future(fshmptr, task_for_queue);
  HSHM_DEVICE_FENCE_SYSTEM();
  auto &qlane = ipc->gpu_info_.gpu2cpu_queue->GetLane(0, 0);
  qlane.Push(task_future);
  return future;
}
#endif  // HSHM_IS_GPU_COMPILER || HSHM_IS_SYCL_COMPILER

#if HSHM_IS_GPU_COMPILER || HSHM_IS_SYCL_COMPILER
/**
 * GPU-side Wait.
 *
 * Polls FUTURE_COMPLETE via volatile read on the FutureShm. Backend is
 * pinned host or UVM, so the CPU's system-scope SetBitsSystem write is
 * visible to a device-side volatile read through PCIe cache snooping.
 */
template <typename TaskT, typename AllocT>
HSHM_CROSS_FUN void gpu::Future<TaskT, AllocT>::Wait() {
#if HSHM_IS_GPU || HSHM_IS_SYCL_DEVICE
#if HSHM_IS_GPU_COMPILER
  if (threadIdx.x != 0) return;
#endif
  if (future_shm_.IsNull()) return;
  gpu::FutureShm *fshm = GetFutureShmPtrRaw();
  if (!fshm) return;
  volatile unsigned int *fp =
      reinterpret_cast<volatile unsigned int *>(&fshm->flags_.bits_.x);
  while (!((*fp) & gpu::FutureShm::FUTURE_COMPLETE)) {}
  HSHM_DEVICE_FENCE_SYSTEM();
#endif
}
#endif  // HSHM_IS_GPU_COMPILER || HSHM_IS_SYCL_COMPILER

}  // namespace chi

#endif  // HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM || HSHM_ENABLE_SYCL
#endif  // CHIMAERA_INCLUDE_CHIMAERA_IPC_GPU2CPU_IMPL_H_

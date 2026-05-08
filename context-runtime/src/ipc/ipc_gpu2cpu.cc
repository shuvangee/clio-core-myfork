/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

#include "chimaera/ipc/ipc_gpu2cpu.h"

#if HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM || HSHM_ENABLE_SYCL

#include "chimaera/ipc_manager.h"
#include "chimaera/gpu/future.h"
#include "chimaera/gpu/gpu_ipc_manager.h"

namespace chi {

/**
 * RuntimeRecv: producer-only — the GPU never serializes a task through
 * lightbeam. Worker::ProcessNewTaskGpu already wrapped the popped task
 * pointer in a chi::Future<Task>; we just hand it back. The
 * `recv_transport` parameter is unused (kept for signature parity with
 * the other transports).
 */
hipc::FullPtr<Task> IpcGpu2Cpu::RuntimeRecv(
    IpcManager *ipc, Future<Task> &future, Container *container,
    u32 method_id, hshm::lbm::Transport *recv_transport) {
  (void)ipc; (void)container; (void)method_id; (void)recv_transport;
  return future.GetTaskPtr();
}

/**
 * RuntimeSend: signal completion on the device-side gpu::FutureShm so the
 * kernel poll-loop unblocks. For kPinnedHost / kManagedUvm backends the
 * gpu_fshm pointer in task_device_ptr_ is directly writable; for
 * kDeviceMem this would issue a cudaMemcpy of the FUTURE_COMPLETE flag —
 * left as a TODO until the device-mem backend kind is fully wired up.
 */
void IpcGpu2Cpu::RuntimeSend(
    IpcManager *ipc, const FullPtr<Task> &task_ptr,
    RunContext *run_ctx, Container *container) {
  (void)container;
  auto future_shm = run_ctx->future_.GetFutureShm();
  HLOG(kInfo, "IpcGpu2Cpu::RuntimeSend: pool={} method={}",
       task_ptr->pool_id_, task_ptr->method_);

  if (future_shm->task_device_ptr_) {
    auto *gpu_fshm = reinterpret_cast<gpu::FutureShm *>(
        future_shm->task_device_ptr_);
    volatile u32 *flags_ptr = reinterpret_cast<volatile u32 *>(
        &gpu_fshm->flags_.bits_.x);
    __sync_fetch_and_or(flags_ptr, gpu::FutureShm::FUTURE_COMPLETE);
  }

  // Mark the chi-side future complete for any host-side waiters.
  future_shm->flags_.SetBitsSystem(FutureShm::FUTURE_COMPLETE);

  // Producer-only model: the client owns the device-memory backend that
  // holds the task — the runtime does not free it. The legacy
  // RoundRobinAllocator-on-device cleanup is gone.
  task_ptr->ClearFlags(TASK_DATA_OWNER);
  (void)ipc;
}

}  // namespace chi

#endif  // HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM || HSHM_ENABLE_SYCL

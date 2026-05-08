/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 * BSD 3-Clause License. See LICENSE file.
 */

/**
 * Shared host-side fixture for the GPU producer-only stress test.
 *
 * Three flavors of this test exist — one per GPU backend — and all of
 * them go through the same producer-only flow:
 *
 *   1. CHIMAERA_INIT(kServer) brings up the CPU runtime; ServerInit
 *      enumerates GPUs and allocates one per-device gpu2cpu_queue.
 *   2. Create a MOD_NAME pool whose CPU-side container handles
 *      `GpuSubmit` (formula: result = test_value * 2 + gpu_id).
 *   3. AllocateAndRegisterGpuBackend(gpu_id, kPinnedHost, bytes, &base)
 *      reserves a pinned-host buffer and registers it with the runtime.
 *   4. Host placement-news N (Task + FutureShm) pairs back-to-back at
 *      stride `sizeof(TaskT) + sizeof(FutureShm)` inside the backend.
 *   5. Each backend launches its own kernel that, per task slot,
 *      mutates the POD test_value, calls CHI_IPC->Send(task_fp), then
 *      future.Wait() on the FutureShm. Concurrent submissions stress
 *      the multi-producer gpu2cpu_queue.
 *   6. The host walks all N tasks and verifies result_value_ matches
 *      the chimod's formula for that slot's input.
 *
 * Tests in this file are gated by the relevant `HSHM_ENABLE_*` macros
 * so each backend's source file (test_gpu_kernel_stress_gpu.cc for
 * CUDA/ROCm, test_gpu_kernel_stress_sycl.cc for SYCL) compiles only
 * when its backend is on.
 */

#ifndef CHIMAERA_TEST_UNIT_GPU_TEST_GPU_KERNEL_STRESS_COMMON_H_
#define CHIMAERA_TEST_UNIT_GPU_TEST_GPU_KERNEL_STRESS_COMMON_H_

#include <chimaera/chimaera.h>
#include <chimaera/singletons.h>
#include <chimaera/types.h>
#include <chimaera/pool_query.h>
#include <chimaera/gpu/future.h>
#include <chimaera/gpu/gpu_info.h>
#include <chimaera/gpu/gpu_ipc_manager.h>
#include <chimaera/MOD_NAME/MOD_NAME_tasks.h>
#include <chimaera/MOD_NAME/MOD_NAME_client.h>

#include <chrono>
#include <cstdio>
#include <new>
#include <thread>
#include <vector>

namespace chi_test_gpu_stress {

/** Number of tasks submitted from the GPU per test run. */
inline constexpr chi::u32 kNumTasks = 64;

/** Stride between adjacent (TaskT + FutureShm) pairs in the backend. */
inline constexpr size_t kSlotBytes =
    sizeof(chimaera::MOD_NAME::GpuSubmitTask) + sizeof(chi::gpu::FutureShm);

/** Total backend size: kNumTasks slots + a few extra bytes of slack. */
inline constexpr size_t kBackendBytes =
    static_cast<size_t>(kNumTasks) * kSlotBytes + 256;

/** Pool ID for the MOD_NAME pool used by the stress test. */
inline chi::PoolId GetTestPoolId() {
  return chi::PoolId(20002, 1);
}

/** One-time runtime setup shared by every TEST_CASE in this binary. */
inline void EnsureInit() {
  static bool initialized = false;
  if (initialized) return;
  std::fprintf(stderr,
               "[INIT] Starting Chimaera server (producer-only GPU)\n");
  if (!chi::CHIMAERA_INIT(chi::ChimaeraMode::kServer)) {
    std::fprintf(stderr, "[INIT] CHIMAERA_INIT failed\n");
    std::abort();
  }
  initialized = true;

  auto *ipc = CHI_CPU_IPC;
  if (!ipc || !ipc->GetGpuIpcManager() ||
      ipc->GetGpuQueueCount() < 1u) {
    std::fprintf(stderr, "[INIT] no GPU queues registered\n");
    std::abort();
  }

  static chimaera::MOD_NAME::Client client(GetTestPoolId());
  using CreateTask = chimaera::MOD_NAME::CreateTask;
  using CreateParams = chimaera::MOD_NAME::CreateParams;
  auto task = ipc->NewTask<CreateTask>(
      chi::CreateTaskId(), chi::kAdminPoolId,
      chi::PoolQuery::Dynamic(),
      CreateParams::chimod_lib_name,
      std::string("gpu_kernel_stress_pool"),
      GetTestPoolId(), &client);
  auto future = ipc->Send(task);
  future.Wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

/**
 * Lay out N (TaskT + FutureShm) pairs in `base` and return the
 * `kNumTasks` task FullPtrs the kernel will iterate over. Slot `i`
 * receives `test_value = i`; the chimod formula then gives
 * `result = i * 2 + gpu_id`.
 *
 * @param base    Pinned-host base of the registered backend.
 * @param alloc_id  AllocatorId returned by AllocateAndRegisterGpuBackend.
 * @param gpu_id  Logical device id (carried into the test_value -> result
 *                formula and used by the chimod).
 * @return        Vector of task FullPtrs (each carries {alloc_id, off}).
 */
inline std::vector<hipc::FullPtr<chimaera::MOD_NAME::GpuSubmitTask>>
PlaceTaskSlots(char *base, hipc::AllocatorId alloc_id, chi::u32 gpu_id) {
  using TaskT = chimaera::MOD_NAME::GpuSubmitTask;
  std::vector<hipc::FullPtr<TaskT>> handles;
  handles.reserve(kNumTasks);
  for (chi::u32 i = 0; i < kNumTasks; ++i) {
    size_t task_off = static_cast<size_t>(i) * kSlotBytes;
    char *task_addr = base + task_off;
    char *fshm_addr = task_addr + sizeof(TaskT);
    auto *task = new (task_addr) TaskT(
        chi::CreateTaskId(), GetTestPoolId(),
        chi::PoolQuery::ToLocalCpu(), gpu_id, /*test_value=*/i);
    task->pod_size_ = static_cast<chi::u32>(sizeof(TaskT));
    new (fshm_addr) chi::gpu::FutureShm();

    hipc::FullPtr<TaskT> fp;
    fp.shm_.alloc_id_ = alloc_id;
    fp.shm_.off_ = task_off;
    fp.ptr_ = task;
    handles.push_back(fp);
  }
  return handles;
}

/**
 * Walk every placed task and verify the chimod's formula. Slot `i` had
 * `test_value = i`, so `result_value_` must equal `i * 2 + gpu_id`.
 * Returns the index of the first mismatch, or kNumTasks on success.
 */
inline chi::u32 VerifyResults(
    const std::vector<hipc::FullPtr<chimaera::MOD_NAME::GpuSubmitTask>>
        &handles,
    chi::u32 gpu_id) {
  for (chi::u32 i = 0; i < handles.size(); ++i) {
    chi::u32 expected = (i * 2u) + gpu_id;
    chi::u32 actual = handles[i]->result_value_;
    if (actual != expected) {
      std::fprintf(stderr,
                   "[CHECK] slot=%u expected=%u actual=%u\n",
                   i, expected, actual);
      return i;
    }
  }
  return static_cast<chi::u32>(handles.size());
}

}  // namespace chi_test_gpu_stress

#endif  // CHIMAERA_TEST_UNIT_GPU_TEST_GPU_KERNEL_STRESS_COMMON_H_

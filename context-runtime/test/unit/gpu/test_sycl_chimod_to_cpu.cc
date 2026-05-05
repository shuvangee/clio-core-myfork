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
 * SYCL kernel → CPU runtime end-to-end test.
 *
 * Validates the simplified SYCL design:
 *   1. Start the Chimaera CPU runtime (CHIMAERA_INIT(kServer)).
 *   2. Create a MOD_NAME pool — its CPU-side container holds the
 *      GpuSubmit handler (MOD_NAME::Runtime::GpuSubmit).
 *   3. Allocate the SYCL gpu2cpu_queue + gpu2cpu_backend (done by
 *      IpcManager::ServerInit's SYCL branch via
 *      gpu::IpcManager::ServerInitGpuQueuesSycl).
 *   4. Submit a SYCL single_task that:
 *        - Binds g_ipc_manager_ptr via CHIMAERA_GPU_INIT.
 *        - Allocates a MOD_NAME::GpuSubmitTask + adjacent FutureShm in
 *          the gpu2cpu_backend (CHI_IPC->NewTask).
 *        - Routes the task to the CPU runtime via PoolQuery::ToLocalCpu()
 *          (CHI_IPC->Send -> IpcGpu2Cpu::ClientSend -> gpu2cpu_queue push).
 *        - Polls FUTURE_COMPLETE (future.Wait()).
 *        - Writes the task's result_value_ to a pinned-host result slot.
 *   5. Host verifies the slot equals (test_value * 2 + gpu_id), the
 *      formula computed by MOD_NAME::Runtime::GpuSubmit.
 *
 * Anything earlier in this pipeline that doesn't really work — host-side
 * queue allocation, kernel-side allocator, push, CPU worker pop and
 * dispatch, completion signal — manifests as the result slot staying 0
 * and the test timing out.
 */

#if HSHM_ENABLE_SYCL && !(HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM)

#include "simple_test.h"

#include <chimaera/chimaera.h>
#include <chimaera/singletons.h>
#include <chimaera/types.h>
#include <chimaera/pool_query.h>
#include <chimaera/gpu/future.h>
#include <chimaera/gpu/gpu_info.h>
#include <chimaera/gpu/gpu_ipc_manager.h>
#include <chimaera/MOD_NAME/MOD_NAME_client.h>
#include <chimaera/MOD_NAME/MOD_NAME_tasks.h>

#include <hermes_shm/util/gpu_api.h>

#include <sycl/sycl.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool g_initialized = false;
chi::PoolId g_pool_id(20000, 1);

/**
 * Initialize the Chimaera CPU runtime once and create a MOD_NAME pool.
 * Re-entrant: subsequent calls are no-ops. Mirrors test_gpu_task_trace's
 * EnsureInit but with the SYCL gpu2cpu queue setup baked into ServerInit.
 */
void EnsureInit() {
  if (g_initialized) return;

  std::fprintf(stderr, "[INIT] Starting Chimaera server (SYCL backend)...\n");
  REQUIRE(chi::CHIMAERA_INIT(chi::ChimaeraMode::kServer));
  g_initialized = true;

  auto *ipc = CHI_CPU_IPC;
  REQUIRE(ipc != nullptr);
  REQUIRE(ipc->GetGpuIpcManager() != nullptr);
  // Server init must have allocated the SYCL gpu2cpu_queue.
  REQUIRE(ipc->GetGpuQueueCount() == 1u);

  // Create the MOD_NAME pool. AsyncCreate is invoked through the CPU
  // admin client — same path the CUDA test uses.
  std::fprintf(stderr, "[INIT] Creating MOD_NAME pool (%u,%u)\n",
               g_pool_id.major_, g_pool_id.minor_);
  static chimaera::MOD_NAME::Client client(g_pool_id);
  using CreateTask = chimaera::MOD_NAME::CreateTask;
  using CreateParams = chimaera::MOD_NAME::CreateParams;
  auto task = ipc->NewTask<CreateTask>(
      chi::CreateTaskId(), chi::kAdminPoolId,
      chi::PoolQuery::Dynamic(),
      CreateParams::chimod_lib_name,
      std::string("sycl_chimod_to_cpu_pool"),
      g_pool_id, &client);
  auto future = ipc->Send(task);
  future.Wait();
  std::this_thread::sleep_for(50ms);
  std::fprintf(stderr, "[INIT] Ready\n");
}

/** Opaque SYCL kernel-name type. Each SYCL submission needs a distinct type. */
class chi_sycl_chimod_to_cpu_kernel;

}  // namespace

TEST_CASE("SYCL bare kernel sanity (no chimaera init)",
          "[sycl][gpu2cpu][bare]") {
  // Pre-init sanity: verify a SYCL kernel can write to malloc_shared
  // without any Chimaera runtime in the picture. If this fails, the
  // problem is with our SYCL queue / USM setup, not the runtime
  // integration. If it passes, the issue is something Chimaera does
  // (signal handlers, pthread state, etc.) that breaks SYCL.
  sycl::queue bare_q{sycl::gpu_selector_v};
  int *bare_done = sycl::malloc_shared<int>(1, bare_q);
  REQUIRE(bare_done != nullptr);
  *bare_done = 0;
  bare_q.submit([&](sycl::handler &cgh) {
    cgh.single_task<class chi_sycl_bare_sanity>([=]() {
      *bare_done = 0xBEEF;
    });
  }).wait_and_throw();
  std::fprintf(stderr, "[BARE] done=%d (expected 0xBEEF=48879)\n", *bare_done);
  REQUIRE(*bare_done == 0xBEEF);
  sycl::free(bare_done, bare_q);
}

TEST_CASE("SYCL → CPU MOD_NAME::GpuSubmit",
          "[sycl][gpu2cpu][mod_name]") {
  EnsureInit();
  auto *ipc = CHI_CPU_IPC;

  // Pull the populated IpcManagerGpuInfo (gpu2cpu_queue + gpu2cpu_backend)
  // from the SYCL backend.
  chi::IpcManagerGpuInfo gpu_info =
      ipc->GetGpuIpcManager()->CreateGpuAllocator(/*size=*/0, /*gpu_id=*/0);
  REQUIRE(gpu_info.gpu2cpu_queue != nullptr);
  REQUIRE(gpu_info.gpu2cpu_backend.data_ != nullptr);

  // SYCL forbids capturing non-trivially-copyable types (IpcManagerGpuInfo
  // holds a MemoryBackend) by value in a kernel lambda. Stage the gpu_info
  // and a kernel-scope gpu::IpcManager in shared USM and capture pointers;
  // the kernel reads/writes through them directly.
  //
  // Use a fresh GPU queue rather than hshm::GpuApi::SyclQueue() (the
  // singleton). The singleton was used for ServerInitGpuQueuesSycl during
  // CHIMAERA_INIT, and on the DPC++ CUDA backend subsequent kernel
  // submissions on it sometimes silently no-op — kernel submitted,
  // wait_and_throw returns clean, but malloc_shared writes don't
  // propagate. A fresh queue avoids that.
  sycl::queue q{sycl::gpu_selector_v};
  // Shared USM (unified memory) for all kernel-visible state. malloc_host
  // is pinned host memory accessible from device by PCIe, but the DPC++
  // CUDA backend needs malloc_shared for transparent bidirectional access
  // when both host and device read/write the same locations.
  auto *info_storage = sycl::malloc_shared<chi::IpcManagerGpuInfo>(1, q);
  REQUIRE(info_storage != nullptr);
  std::memcpy(static_cast<void *>(info_storage), &gpu_info,
              sizeof(chi::IpcManagerGpuInfo));

  auto *ipc_storage = sycl::malloc_shared<chi::gpu::IpcManager>(1, q);
  REQUIRE(ipc_storage != nullptr);
  new (ipc_storage) chi::gpu::IpcManager();

  auto *d_done = sycl::malloc_shared<int>(1, q);
  *d_done = 0;
  auto *d_result = sycl::malloc_shared<uint32_t>(1, q);
  *d_result = 0;

  const chi::u32 test_value = 42;
  const chi::u32 gpu_id = 0;
  chi::PoolId pool_id = g_pool_id;
  uint32_t *result_ptr = d_result;
  int *done_ptr = d_done;

  std::fprintf(stderr,
               "[TRACE] Launching SYCL kernel (test_value=%u, gpu_id=%u)\n",
               test_value, gpu_id);

  // End-to-end kernel: CHIMAERA_GPU_INIT binds the kernel-scope IpcManager,
  // NewTask allocates a GpuSubmitTask + FutureShm in the gpu2cpu_backend,
  // Send pushes onto gpu2cpu_queue (PoolQuery::ToLocalCpu), Wait polls the
  // FutureShm FUTURE_COMPLETE bit. The CPU worker
  // (Worker::ProcessNewTaskGpu) pops, runs MOD_NAME::Runtime::GpuSubmit
  // (which sets task->result_value_ = test_value*2 + gpu_id), then
  // IpcGpu2Cpu::RuntimeSend marks FUTURE_COMPLETE.
  //
  // The unconditional `*result_ptr=1` at the top is required: DPC++ elides
  // the kernel emission when the lambda body's only host-pass content is
  // (void)cast no-ops, so keep at least one unconditional non-trivial
  // statement using the captured pointer.
  q.submit([&](sycl::handler &cgh) {
    cgh.single_task<chi_sycl_chimod_to_cpu_kernel>([=]() {
      // Unconditional non-trivial use of result_ptr is required: DPC++
      // elides kernel emission when the lambda body's only host-pass
      // content is (void)cast no-ops. With this write, the kernel emits.
      *result_ptr = 1;
#if HSHM_IS_DEVICE_PASS
      // Force the device pass to load info_storage->backend.data_ before
      // CHIMAERA_GPU_INIT runs. Without this load, DPC++'s optimizer leaves
      // gpu_alloc_ unset after init (observed: NewTask returns null).
      // Hypothesis: ClientInitGpu reads info_storage via const-ref, and
      // unless we touch the same field through info_storage at the kernel
      // level the optimizer omits the underlying USM load.
      if (info_storage->backend.data_ != nullptr) {
        auto *pre_alloc = reinterpret_cast<hipc::RoundRobinAllocator *>(
            info_storage->backend.data_);
        (void)pre_alloc->heap_ready_.load();
      }
      CHIMAERA_GPU_INIT(*info_storage, ipc_storage);
      *result_ptr = 2;
      auto task = CHI_IPC->NewTask<chimaera::MOD_NAME::GpuSubmitTask>(
          chi::CreateTaskId(), pool_id,
          chi::PoolQuery::ToLocalCpu(),
          gpu_id, test_value);
      if (!task.IsNull()) {
        *result_ptr = 3;
        auto future = CHI_IPC->Send(task);
        *result_ptr = 4;
        future.Wait();
        *result_ptr = task->result_value_;
      } else {
        *result_ptr = 99;
      }
      (void)g_ipc_manager;
#else
      // Host-pass: touch every captured non-pointer value to make sure
      // DPC++'s host-side capture analysis lays out the kernel argument
      // tuple identically to the device pass. Without this, observed:
      // kernel arg layout mismatch ("Unexpected kernel lambda size") OR
      // device pass emits empty kernel.
      (void)info_storage; (void)ipc_storage;
      (void)pool_id; (void)gpu_id; (void)test_value;
#endif
      *done_ptr = 1;
    });
  }).wait_and_throw();

  // Poll the result slot. We use a separate poll loop after wait_and_throw
  // so the host sees the writes through the USM-host coherence path.
  auto t0 = std::chrono::steady_clock::now();
  while (*d_done == 0) {
    std::this_thread::sleep_for(100us);
    float elapsed = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (elapsed > 5.0f) break;
  }
  float ms = std::chrono::duration<float, std::milli>(
                 std::chrono::steady_clock::now() - t0)
                 .count();

  chi::u32 result = *d_result;
  chi::u32 expected = (test_value * 2) + gpu_id;
  std::fprintf(stderr,
               "[TRACE] done=%d result=%u expected=%u elapsed=%.2f ms\n",
               *d_done, result, expected, ms);

  REQUIRE(*d_done == 1);
  // Full pipeline: result_value_ should equal MOD_NAME::Runtime::GpuSubmit's
  // formula (test_value * 2 + gpu_id).
  REQUIRE(result == expected);

  // Cleanup
  ipc_storage->~IpcManager();
  sycl::free(static_cast<void *>(ipc_storage), q);
  sycl::free(static_cast<void *>(info_storage), q);
  sycl::free(d_done, q);
  sycl::free(d_result, q);
}

SIMPLE_TEST_MAIN()

#else  // !HSHM_ENABLE_SYCL or CUDA/ROCm path

int main() { return 0; }

#endif  // HSHM_ENABLE_SYCL && !(CUDA||ROCM)

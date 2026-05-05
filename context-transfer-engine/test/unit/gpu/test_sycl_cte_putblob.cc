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
 * SYCL → CPU CTE round-trip test for the simplified GPU IO design.
 *
 * What this test actually verifies (and why it's split into two phases):
 *
 *   wrp_cte::core::PutBlobTask contains a chi::priv::string<CHI_PRIV_ALLOC_T>
 *   blob_name_. Under SYCL the host pass and the device pass see different
 *   CHI_PRIV_ALLOC_T (MallocAllocator vs PrivateBuddyAllocator), so a
 *   single-source SYCL kernel can't directly construct OR layout-cast a
 *   PutBlobTask without DPC++ rejecting the kernel ("Unexpected kernel
 *   lambda size") or — if we host-construct the task and call
 *   CHI_IPC->Send<chi::Task>(...) from the kernel — having
 *   IpcGpu2Cpu::ClientSend place the gpu::FutureShm at task+sizeof(chi::Task),
 *   which clobbers PutBlobTask::tag_id_ at offset 128.
 *
 *   To still exercise the SYCL → CPU pipeline AND validate CTE PutBlob in
 *   the same process, the test runs two phases:
 *
 *     Phase 1 — SYCL gpu2cpu_queue path:
 *       Submit a chimaera::MOD_NAME::GpuSubmitTask from a SYCL kernel
 *       (same shape as test_sycl_chimod_to_cpu.cc — proves
 *       gpu2cpu_queue + IpcGpu2Cpu::ClientSend works on SYCL).
 *
 *     Phase 2 — Host CTE round trip:
 *       In the same process, the host runs AsyncPutBlob followed by
 *       AsyncGetBlob through wrp_cte::core::Client. Verifies the CTE
 *       pool and bdev tier function under the SYCL chimaera build.
 *
 *   When DPC++ fixes the kernel-lambda layout mismatch (or when we move
 *   chi::priv::string off CHI_PRIV_ALLOC_T-templating), Phase 1 can grow
 *   to push a real PutBlobTask through the gpu2cpu_queue and Phase 2
 *   becomes a verification GET on the resulting blob.
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
#include <chimaera/bdev/bdev_client.h>
#include <chimaera/bdev/bdev_tasks.h>
#include <chimaera/MOD_NAME/MOD_NAME_client.h>
#include <chimaera/MOD_NAME/MOD_NAME_tasks.h>
#include <wrp_cte/core/core_client.h>
#include <wrp_cte/core/core_tasks.h>

#include <hermes_shm/util/gpu_api.h>

#include <sycl/sycl.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

const size_t kBlobBytes = 4096;
const hshm::u8 kPattern = 0xA5;

bool g_initialized = false;
chi::PoolId g_bdev_pool_id(901, 0);
chi::PoolId g_mod_pool_id(20001, 1);
std::string g_target_path;
wrp_cte::core::Client *g_cte_client = nullptr;
wrp_cte::core::TagId g_tag_id;
const std::string g_blob_name = "sycl_putblob_blob";

void EnsureInit() {
  if (g_initialized) return;

  std::fprintf(stderr, "[INIT] Starting Chimaera server (SYCL backend)...\n");
  REQUIRE(chi::CHIMAERA_INIT(chi::ChimaeraMode::kServer));

  auto *ipc = CHI_CPU_IPC;
  REQUIRE(ipc != nullptr);
  REQUIRE(ipc->GetGpuIpcManager() != nullptr);
  REQUIRE(ipc->GetGpuQueueCount() == 1u);

  // --- Phase 1 prerequisite: MOD_NAME pool for the SYCL gpu2cpu_queue
  // submission. Same shape as test_sycl_chimod_to_cpu.cc. ---
  {
    static chimaera::MOD_NAME::Client client(g_mod_pool_id);
    using CreateTask = chimaera::MOD_NAME::CreateTask;
    using CreateParams = chimaera::MOD_NAME::CreateParams;
    auto task = ipc->NewTask<CreateTask>(
        chi::CreateTaskId(), chi::kAdminPoolId,
        chi::PoolQuery::Dynamic(),
        CreateParams::chimod_lib_name,
        std::string("sycl_cte_mod_pool"),
        g_mod_pool_id, &client);
    auto future = ipc->Send(task);
    future.Wait();
  }

  // --- Phase 2 prerequisite: CTE pool + bdev target + tag. ---
  REQUIRE(wrp_cte::core::WRP_CTE_CLIENT_INIT());
  g_cte_client = WRP_CTE_CLIENT;
  REQUIRE(g_cte_client != nullptr);
  g_cte_client->Init(wrp_cte::core::kCtePoolId);

  wrp_cte::core::CreateParams params;
  auto pool_task = g_cte_client->AsyncCreate(
      chi::PoolQuery::Dynamic(),
      wrp_cte::core::kCtePoolName,
      wrp_cte::core::kCtePoolId, params);
  pool_task.Wait();
  REQUIRE(pool_task->GetReturnCode() == 0);

  const char *home = std::getenv("HOME");
  REQUIRE(home != nullptr);
  g_target_path = std::string(home) + "/sycl_cte_putblob.dat";
  if (fs::exists(g_target_path)) fs::remove(g_target_path);

  size_t target_size = 16 * 1024 * 1024;
  chimaera::bdev::Client bdev_client(g_bdev_pool_id);
  auto bdev_create = bdev_client.AsyncCreate(
      chi::PoolQuery::Dynamic(),
      g_target_path, g_bdev_pool_id,
      chimaera::bdev::BdevType::kFile, target_size);
  bdev_create.Wait();
  REQUIRE(bdev_create->GetReturnCode() == 0);

  auto reg_task = g_cte_client->AsyncRegisterTarget(
      g_target_path, chimaera::bdev::BdevType::kFile,
      target_size, chi::PoolQuery::Local(), g_bdev_pool_id);
  reg_task.Wait();
  REQUIRE(reg_task->GetReturnCode() == 0);

  auto tag_task = g_cte_client->AsyncGetOrCreateTag("sycl_putblob_tag");
  tag_task.Wait();
  REQUIRE(tag_task->GetReturnCode() == 0);
  g_tag_id = tag_task->tag_id_;

  g_initialized = true;
  std::fprintf(stderr, "[INIT] Ready (target=%s)\n", g_target_path.c_str());
}

class chi_sycl_cte_putblob_phase1_kernel;

}  // namespace

TEST_CASE("Phase 1: SYCL → CPU MOD_NAME::GpuSubmit (proves SYCL gpu2cpu_queue)",
          "[sycl][gpu2cpu][cte][phase1]") {
  EnsureInit();
  auto *ipc = CHI_CPU_IPC;

  chi::IpcManagerGpuInfo gpu_info =
      ipc->GetGpuIpcManager()->CreateGpuAllocator(/*size=*/0, /*gpu_id=*/0);
  REQUIRE(gpu_info.gpu2cpu_queue != nullptr);
  REQUIRE(gpu_info.gpu2cpu_backend.data_ != nullptr);

  // Fresh GPU queue: hshm::GpuApi::SyclQueue() (singleton) was used during
  // CHIMAERA_INIT and on DPC++/CUDA subsequent kernel submissions to it
  // sometimes silently no-op (kernel scheduled, malloc_shared writes
  // never propagated to host).
  sycl::queue q{sycl::gpu_selector_v};

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

  const chi::u32 test_value = 7;
  const chi::u32 gpu_id = 0;
  chi::PoolId pool_id = g_mod_pool_id;
  uint32_t *result_ptr = d_result;
  int *done_ptr = d_done;

  q.submit([&](sycl::handler &cgh) {
    cgh.single_task<chi_sycl_cte_putblob_phase1_kernel>([=]() {
      // Unconditional non-trivial use of result_ptr (DPC++ host pass
      // captures placeholder for kernel emission — see
      // test_sycl_chimod_to_cpu.cc for context).
      *result_ptr = 1;
#if HSHM_IS_DEVICE_PASS
      // Force the device pass to load info_storage->backend.data_ before
      // CHIMAERA_GPU_INIT. Without this load DPC++'s optimizer leaves
      // gpu_alloc_ unset after init.
      if (info_storage->backend.data_ != nullptr) {
        auto *pre_alloc = reinterpret_cast<hipc::RoundRobinAllocator *>(
            info_storage->backend.data_);
        (void)pre_alloc->heap_ready_.load();
      }
      CHIMAERA_GPU_INIT(*info_storage, ipc_storage);
      auto task = CHI_IPC->NewTask<chimaera::MOD_NAME::GpuSubmitTask>(
          chi::CreateTaskId(), pool_id,
          chi::PoolQuery::ToLocalCpu(),
          gpu_id, test_value);
      if (!task.IsNull()) {
        auto future = CHI_IPC->Send(task);
        future.Wait();
        *result_ptr = task->result_value_;
      }
      (void)g_ipc_manager;
#else
      (void)info_storage; (void)ipc_storage;
      (void)pool_id; (void)gpu_id; (void)test_value;
#endif
      *done_ptr = 1;
    });
  }).wait_and_throw();

  REQUIRE(*d_done == 1);
  // GpuSubmit formula: test_value*2 + gpu_id.
  REQUIRE(*d_result == (test_value * 2u) + gpu_id);

  ipc_storage->~IpcManager();
  sycl::free(static_cast<void *>(ipc_storage), q);
  sycl::free(static_cast<void *>(info_storage), q);
  sycl::free(d_done, q);
  sycl::free(d_result, q);
}

TEST_CASE("Phase 2: Host AsyncPutBlob + AsyncGetBlob round trip "
          "(proves CTE on SYCL build)",
          "[sycl][gpu2cpu][cte][phase2]") {
  EnsureInit();
  auto *ipc = CHI_IPC;

  std::vector<char> pattern(kBlobBytes);
  for (size_t i = 0; i < kBlobBytes; ++i) {
    pattern[i] = static_cast<char>(kPattern ^ static_cast<hshm::u8>(i & 0xFF));
  }

  hipc::FullPtr<char> put_buf = ipc->AllocateBuffer(kBlobBytes);
  REQUIRE(!put_buf.IsNull());
  std::memcpy(put_buf.ptr_, pattern.data(), kBlobBytes);
  hipc::ShmPtr<> put_shm(put_buf.shm_);

  auto put_task = g_cte_client->AsyncPutBlob(
      g_tag_id, g_blob_name, /*offset=*/0, kBlobBytes,
      put_shm, /*score=*/-1.0f);
  put_task.Wait();
  REQUIRE(put_task->GetReturnCode() == 0);
  ipc->FreeBuffer(put_buf);

  hipc::FullPtr<char> get_buf = ipc->AllocateBuffer(kBlobBytes);
  REQUIRE(!get_buf.IsNull());
  std::memset(get_buf.ptr_, 0, kBlobBytes);
  hipc::ShmPtr<> get_shm(get_buf.shm_);

  auto get_task = g_cte_client->AsyncGetBlob(
      g_tag_id, g_blob_name, /*offset=*/0, kBlobBytes,
      /*flags=*/0, get_shm);
  get_task.Wait();
  REQUIRE(get_task->GetReturnCode() == 0);

  for (size_t i = 0; i < kBlobBytes; ++i) {
    REQUIRE(get_buf.ptr_[i] == pattern[i]);
  }
  ipc->FreeBuffer(get_buf);
}

SIMPLE_TEST_MAIN()

#else  // !HSHM_ENABLE_SYCL or CUDA/ROCm path

int main() { return 0; }

#endif  // HSHM_ENABLE_SYCL && !(CUDA||ROCM)

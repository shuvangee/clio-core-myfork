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

#ifndef SAFE_BDEV_CLIENT_H_
#define SAFE_BDEV_CLIENT_H_

#include <clio_runtime/clio_runtime.h>

#include <string>
#include <vector>

#include "safe_bdev_tasks.h"

/**
 * Client API for safe_bdev ChiMod
 *
 * Provides the client interface for the declustered, erasure-coded block
 * device. Data-plane calls reuse the bdev task types; management calls drive
 * membership and parity maintenance.
 */

namespace clio::run::safe_bdev {

class Client : public chi::ContainerClient {
 public:
  CTP_CROSS_FUN Client() = default;
  CTP_CROSS_FUN explicit Client(const chi::PoolId &pool_id) { Init(pool_id); }

  /**
   * Create safe_bdev container - asynchronous.
   * @param pool_query Pool query for routing
   * @param pool_name Pool name for the safe_bdev container
   * @param custom_pool_id Explicit pool ID for the pool being created
   * @param max_failures Number of simultaneous member failures to tolerate
   * @param members Initial member bdev descriptors
   */
  chi::Future<clio::run::safe_bdev::CreateTask> AsyncCreate(
      const chi::PoolQuery &pool_query, const std::string &pool_name,
      const chi::PoolId &custom_pool_id, chi::u32 max_failures,
      const std::vector<MemberBdevDesc> &members) {
    auto *ipc_manager = CLIO_CPU_IPC;

    // CreateTask must always go through the admin pool, never pool_id_.
    auto task = ipc_manager->NewTask<clio::run::safe_bdev::CreateTask>(
        chi::CreateTaskId(),
        chi::kAdminPoolId,  // Send to admin pool for GetOrCreatePool processing
        pool_query,
        CreateParams::chimod_lib_name,  // chimod name from CreateParams
        pool_name,                      // user-provided pool name
        custom_pool_id,                 // target pool ID to create
        this,                           // Client pointer for PostWait
        // CreateParams constructor arguments:
        max_failures, members);

    return ipc_manager->Send(task);
  }

  /**
   * Allocate data blocks - asynchronous.
   */
  chi::Future<AllocateBlocksTask> AsyncAllocateBlocks(
      const chi::PoolQuery &pool_query, chi::u64 size) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<AllocateBlocksTask>(
        chi::CreateTaskId(), pool_id_, pool_query, size);
    return ipc_manager->Send(task);
  }

  /**
   * Free multiple blocks - asynchronous (host, std::vector).
   */
  chi::Future<FreeBlocksTask> AsyncFreeBlocks(
      const chi::PoolQuery &pool_query, const std::vector<Block> &blocks) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<FreeBlocksTask>(
        chi::CreateTaskId(), pool_id_, pool_query, blocks);
    return ipc_manager->Send(task);
  }

  /**
   * Free multiple blocks - asynchronous (priv::vector).
   */
  chi::Future<FreeBlocksTask> AsyncFreeBlocks(
      const chi::PoolQuery &pool_query,
      const chi::priv::vector<Block> &blocks) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<FreeBlocksTask>(
        chi::CreateTaskId(), pool_id_, pool_query, blocks);
    return ipc_manager->Send(task);
  }

  /**
   * Write data to blocks - asynchronous.
   */
  chi::Future<WriteTask> AsyncWrite(const chi::PoolQuery &pool_query,
                                    const chi::priv::vector<Block> &blocks,
                                    ctp::ipc::ShmPtr<> data, size_t length) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<WriteTask>(
        chi::CreateTaskId(), pool_id_, pool_query, blocks, data, length);
    return ipc_manager->Send(task);
  }

  /**
   * Read data from blocks - asynchronous.
   */
  chi::Future<ReadTask> AsyncRead(const chi::PoolQuery &pool_query,
                                  const chi::priv::vector<Block> &blocks,
                                  ctp::ipc::ShmPtr<> data, size_t buffer_size) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<ReadTask>(
        chi::CreateTaskId(), pool_id_, pool_query, blocks, data, buffer_size);
    return ipc_manager->Send(task);
  }

  /**
   * Get performance statistics - asynchronous.
   */
  chi::Future<GetStatsTask> AsyncGetStats() {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<GetStatsTask>(
        chi::CreateTaskId(), pool_id_, chi::PoolQuery());
    return ipc_manager->Send(task);
  }

  /**
   * Monitor container state - asynchronous.
   */
  chi::Future<MonitorTask> AsyncMonitor(const chi::PoolQuery &pool_query,
                                        const std::string &query) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<MonitorTask>(
        chi::CreateTaskId(), pool_id_, pool_query, query);
    return ipc_manager->Send(task);
  }

  /**
   * Add a member bdev - asynchronous.
   */
  chi::Future<AddBdevTask> AsyncAddBdev(const chi::PoolQuery &pool_query,
                                        const std::string &pool_name,
                                        chi::u32 node_id,
                                        const chi::PoolId &member_pool_id,
                                        chi::u32 as_parity = 0) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<AddBdevTask>(
        chi::CreateTaskId(), pool_id_, pool_query, pool_name, node_id,
        member_pool_id, as_parity);
    return ipc_manager->Send(task);
  }

  /**
   * Remove a member bdev - asynchronous.
   */
  chi::Future<RemoveBdevTask> AsyncRemoveBdev(const chi::PoolQuery &pool_query,
                                              const chi::PoolId &target_pool_id,
                                              chi::u32 was_faulty) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<RemoveBdevTask>(
        chi::CreateTaskId(), pool_id_, pool_query, target_pool_id, was_faulty);
    return ipc_manager->Send(task);
  }

  /**
   * Recover a failed member bdev onto a new member - asynchronous.
   */
  chi::Future<RecoverBdevTask> AsyncRecoverBdev(
      const chi::PoolQuery &pool_query, const chi::PoolId &old_bdev_id,
      const std::string &pool_name, chi::u32 node_id,
      const chi::PoolId &new_pool_id) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<RecoverBdevTask>(
        chi::CreateTaskId(), pool_id_, pool_query, old_bdev_id, pool_name,
        node_id, new_pool_id);
    return ipc_manager->Send(task);
  }

  /**
   * Build/raise parity for dirty stripes - asynchronous.
   * @param max_batch  max stripes to (re)build this pass; 0 = drain all.
   * @param period_us  if > 0, register as a periodic background task.
   */
  chi::Future<BuildParityTask> AsyncBuildParity(const chi::PoolQuery &pool_query,
                                                chi::u32 max_batch = 0,
                                                double period_us = 0) {
    auto *ipc_manager = CLIO_CPU_IPC;
    auto task = ipc_manager->NewTask<BuildParityTask>(
        chi::CreateTaskId(), pool_id_, pool_query, max_batch);
    if (period_us > 0) {
      task->SetPeriod(period_us, chi::kMicro);
      task->SetFlags(TASK_PERIODIC);
    }
    return ipc_manager->Send(task);
  }
};

}  // namespace clio::run::safe_bdev

#endif  // SAFE_BDEV_CLIENT_H_

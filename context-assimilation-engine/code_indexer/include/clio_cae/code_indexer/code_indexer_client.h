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

#ifndef CLIO_CAE_CODE_INDEXER_CODE_INDEXER_CLIENT_H_
#define CLIO_CAE_CODE_INDEXER_CODE_INDEXER_CLIENT_H_

#include <clio_cae/code_indexer/code_indexer_tasks.h>
#include <clio_runtime/admin/admin_client.h>
#include <clio_runtime/clio_runtime.h>

#include <string>

namespace clio::cae::code_indexer {

/**
 * Client for the Code Indexer pool.
 *
 *   Client c(clio::cae::code_indexer::kCodeIndexerPoolId);
 *   auto idx = c.AsyncIndexPath("/path/to/src");
 *   idx.Wait();                       // blocks until every file is embedded
 *   auto q = c.AsyncQuery("where are blobs reorganized", 5);
 *   q.Wait();
 *   for (const HitRow &h : q->hits_) { ... }
 */
class Client : public clio::run::ContainerClient {
 public:
  Client() = default;
  explicit Client(const clio::run::PoolId &pool_id) { Init(pool_id); }

  /**
   * Create the pool. After Wait(), call client.Init(task->new_pool_id_).
   */
  clio::run::Future<CreateTask> AsyncCreate(
      const clio::run::PoolQuery &pool_query, const std::string &pool_name,
      const clio::run::PoolId &custom_pool_id,
      const CreateParams &params = CreateParams()) {
    auto *ipc = CLIO_IPC;
    // CreateTask MUST go to the admin pool for GetOrCreatePool processing.
    auto task = ipc->NewTask<CreateTask>(
        clio::run::CreateTaskId(), clio::run::kAdminPoolId, pool_query,
        CreateParams::chimod_lib_name, pool_name, custom_pool_id, this,
        params);
    return ipc->Send(task);
  }

  /** Index every source file under `root` (a path on the pool's node). */
  clio::run::Future<IndexPathTask> AsyncIndexPath(
      const std::string &root, clio::run::u32 max_files = 0) {
    auto *ipc = CLIO_IPC;
    auto task = ipc->NewTask<IndexPathTask>(
        clio::run::CreateTaskId(), pool_id_, clio::run::PoolQuery::Local(),
        root, max_files);
    return ipc->Send(task);
  }

  /** Rank indexed files for `query`; `mode` is a QueryMode::* value. */
  clio::run::Future<QueryTask> AsyncQuery(
      const std::string &query, clio::run::u32 k = 5,
      clio::run::u32 mode = QueryMode::kHybrid) {
    auto *ipc = CLIO_IPC;
    auto task = ipc->NewTask<QueryTask>(
        clio::run::CreateTaskId(), pool_id_, clio::run::PoolQuery::Local(),
        query, k, mode);
    return ipc->Send(task);
  }

  /** Index size. */
  clio::run::Future<StatusTask> AsyncStatus() {
    auto *ipc = CLIO_IPC;
    auto task = ipc->NewTask<StatusTask>(
        clio::run::CreateTaskId(), pool_id_, clio::run::PoolQuery::Local());
    return ipc->Send(task);
  }
};

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CODE_INDEXER_CLIENT_H_

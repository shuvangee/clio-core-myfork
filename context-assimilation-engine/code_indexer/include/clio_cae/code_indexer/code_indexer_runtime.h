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

#ifndef CLIO_CAE_CODE_INDEXER_CODE_INDEXER_RUNTIME_H_
#define CLIO_CAE_CODE_INDEXER_CODE_INDEXER_RUNTIME_H_

#include <clio_cae/code_indexer/code_indexer.h>
#include <clio_cae/code_indexer/code_indexer_client.h>
#include <clio_cae/code_indexer/code_indexer_tasks.h>
#include <clio_runtime/clio_runtime.h>

#include <memory>
#include <mutex>
#include <string>

namespace clio::cae::code_indexer {

/**
 * Container that owns one in-memory CodeIndexer.
 *
 * Concurrency: the index is guarded by `mu_`, taken per FILE during indexing
 * and per Query. A Query issued while a large IndexPath runs therefore sees a
 * partially built index and waits at most one file's summarize+embed latency.
 * IndexPath itself blocks its worker for the whole directory (inference is
 * synchronous libcurl, as in the summarizer), so size num_threads accordingly.
 */
class Runtime : public clio::run::Container {
 public:
  // CreateParams type used by the CLIO_TASK_CC macro for lib_name access.
  using CreateParams = clio::cae::code_indexer::CreateParams;

  Runtime() = default;
  ~Runtime() override = default;

  // ---- Method handlers ----
  clio::run::TaskResume Create(clio::run::shared_ptr<CreateTask> &task);
  clio::run::TaskResume Destroy(clio::run::shared_ptr<DestroyTask> &task);
  clio::run::TaskResume Monitor(clio::run::shared_ptr<MonitorTask> &task);
  clio::run::TaskResume IndexPath(clio::run::shared_ptr<IndexPathTask> &task);
  clio::run::TaskResume Query(clio::run::shared_ptr<QueryTask> &task);
  clio::run::TaskResume Status(clio::run::shared_ptr<StatusTask> &task);

  // ---- Container virtuals (defined in autogen/code_indexer_lib_exec.cc) ----
  void Init(const clio::run::PoolId &pool_id, const std::string &pool_name,
            clio::run::u32 container_id = 0) override;
  clio::run::TaskResume Run(
      clio::run::u32 method,
      clio::run::shared_ptr<clio::run::Task> task_ptr) override;
  clio::run::u64 GetWorkRemaining() const override;
  void SaveTask(clio::run::u32 method, clio::run::SaveTaskArchive &archive,
                clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  void LoadTask(clio::run::u32 method, clio::run::LoadTaskArchive &archive,
                clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> AllocLoadTask(
      clio::run::u32 method, clio::run::LoadTaskArchive &archive) override;
  void LocalLoadTask(clio::run::u32 method,
                     clio::run::DefaultLoadArchive &archive,
                     clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> LocalAllocLoadTask(
      clio::run::u32 method, clio::run::DefaultLoadArchive &archive) override;
  void LocalSaveTask(clio::run::u32 method,
                     clio::run::DefaultSaveArchive &archive,
                     clio::run::shared_ptr<clio::run::Task> &task_ptr) override;
  clio::run::shared_ptr<clio::run::Task> NewCopyTask(
      clio::run::u32 method,
      clio::run::shared_ptr<clio::run::Task> &orig_task_ptr,
      bool deep) override;
  clio::run::shared_ptr<clio::run::Task> NewTask(
      clio::run::u32 method) override;
  void AggregateOut(
      clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> &orig_task,
      const clio::run::shared_ptr<clio::run::Task> &replica_task) override;
  void AggregateIn(
      clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> &agg_task,
      const clio::run::shared_ptr<clio::run::Task> &member_task) override;

 private:
  CreateParams params_;
  std::mutex mu_;
  std::unique_ptr<CodeIndexer> indexer_;
};

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CODE_INDEXER_RUNTIME_H_

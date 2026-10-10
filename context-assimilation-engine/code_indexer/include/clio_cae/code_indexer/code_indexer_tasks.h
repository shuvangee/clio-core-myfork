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

#ifndef CLIO_CAE_CODE_INDEXER_CODE_INDEXER_TASKS_H_
#define CLIO_CAE_CODE_INDEXER_CODE_INDEXER_TASKS_H_

/**
 * Pool configuration and task types of the Code Indexer ChiMod.
 *
 * The pool wraps clio::cae::code_indexer::CodeIndexer (the in-memory
 * summarize -> embed -> search pipeline) behind three verbs:
 *   IndexPath  scan a directory on the pool's node and index it
 *   Query      rank indexed files for a natural-language question
 *   Status     report index size
 */

#include <clio_cae/code_indexer/autogen/code_indexer_methods.h>
#include <clio_ctp/util/msan.h>
#include <clio_runtime/admin/admin_tasks.h>
#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/task.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <string>
#include <vector>

namespace clio::cae::code_indexer {

/** Well-known default pool id / name (400 = CAE core, 401 = summarizer). */
static constexpr clio::run::PoolId kCodeIndexerPoolId(402, 0);
static constexpr const char *kCodeIndexerPoolName = "clio_cae_code_indexer";

/** Search mode wire values; MUST match the SearchMode enum order. */
namespace QueryMode {
constexpr clio::run::u32 kSummary = 0;
constexpr clio::run::u32 kCode = 1;
constexpr clio::run::u32 kHybrid = 2;
constexpr clio::run::u32 kKeyword = 3;
}  // namespace QueryMode

/**
 * Container creation parameters (compose YAML keys match the field names).
 *
 *   embed_endpoint / embed_model / embed_api("ollama"|"openai") / embed_dim /
 *   embed_batch     the embedding backend
 *   summarize_endpoint / summarize_model / summarize_prompt /
 *   summarize_ctx / summarize_num_predict
 *                   the summarizer; an empty summarize_model indexes code
 *                   without summaries
 *   chunk_lines / max_chunk_chars / max_chunks_per_file   chunking
 *   query_prefix / doc_prefix   model-specific retrieval prefixes ("" keeps
 *                   the EmbeddingGemma defaults)
 */
struct CreateParams {
  // Required: chimod library name for the module manager.
  static constexpr const char *chimod_lib_name = "clio_cae_code_indexer";

  std::string embed_endpoint;
  std::string embed_model = "embeddinggemma";
  std::string embed_api = "ollama";
  int embed_dim = 0;
  clio::run::u32 embed_batch = 16;

  std::string summarize_endpoint;
  std::string summarize_model;
  std::string summarize_prompt;
  int summarize_ctx = 8192;
  int summarize_num_predict = 256;

  clio::run::u32 chunk_lines = 60;
  clio::run::u32 max_chunk_chars = 6000;
  clio::run::u32 max_chunks_per_file = 24;

  std::string query_prefix;
  std::string doc_prefix;

  CreateParams() = default;
  CreateParams(const CreateParams &other) = default;

  // Compose pool-id ctor (matches the other chimods).
  CreateParams(const clio::run::PoolId &pool_id, const CreateParams &other)
      : CreateParams(other) {
    (void)pool_id;
  }

  template <class Archive>
  void serialize(Archive &ar) {
    // EVERY field round-trips (the silently-dropped-field audit rule).
    ar(embed_endpoint, embed_model, embed_api, embed_dim, embed_batch,
       summarize_endpoint, summarize_model, summarize_prompt, summarize_ctx,
       summarize_num_predict, chunk_lines, max_chunk_chars,
       max_chunks_per_file, query_prefix, doc_prefix);
  }

  /** Load from the compose entry's YAML. Best-effort, like the summarizer. */
  void LoadConfig(const clio::run::PoolConfig &pool_config) {
    if (pool_config.config_.empty()) return;
    try {
      YAML::Node n = YAML::Load(pool_config.config_);
      ctp::MsanUnpoisonYaml(n);
      auto str = [&](const char *k, std::string *out) {
        if (n[k]) *out = n[k].as<std::string>();
      };
      auto i32 = [&](const char *k, int *out) {
        if (n[k]) *out = n[k].as<int>();
      };
      auto u32 = [&](const char *k, clio::run::u32 *out) {
        if (n[k]) *out = n[k].as<clio::run::u32>();
      };
      str("embed_endpoint", &embed_endpoint);
      str("embed_model", &embed_model);
      str("embed_api", &embed_api);
      i32("embed_dim", &embed_dim);
      u32("embed_batch", &embed_batch);
      str("summarize_endpoint", &summarize_endpoint);
      str("summarize_model", &summarize_model);
      str("summarize_prompt", &summarize_prompt);
      i32("summarize_ctx", &summarize_ctx);
      i32("summarize_num_predict", &summarize_num_predict);
      u32("chunk_lines", &chunk_lines);
      u32("max_chunk_chars", &max_chunk_chars);
      u32("max_chunks_per_file", &max_chunks_per_file);
      str("query_prefix", &query_prefix);
      str("doc_prefix", &doc_prefix);
    } catch (...) {
      // Config parsing is best-effort
    }
  }
};

/** Standard pool creation. */
using CreateTask = clio::run::admin::GetOrCreatePoolTask<CreateParams>;

/** Cleanup the container. */
struct DestroyTask : public clio::run::Task {
  DestroyTask() : clio::run::Task() {}

  explicit DestroyTask(const clio::run::TaskId &task_id,
                       const clio::run::PoolId &pool_id,
                       const clio::run::PoolQuery &pool_query)
      : clio::run::Task(task_id, pool_id, pool_query, Method::kDestroy) {}

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task> &other_base) {
    // No OUT fields: the base merge (return code + completer) is everything.
    Task::AggregateOut(other_base);
  }

  void Copy(const ctp::ipc::FullPtr<DestroyTask> &other) {
    Task::Copy(other.template Cast<clio::run::Task>());
  }

  template <typename Ar>
  void SerializeIn(Ar &ar) {
    Task::SerializeIn(ar);
  }
  template <typename Ar>
  void SerializeOut(Ar &ar) {
    Task::SerializeOut(ar);
  }
};

using MonitorTask = clio::run::admin::MonitorTask;

/** One ranked file in a QueryTask result. */
struct HitRow {
  std::string path_;
  float score_ = 0.0f;
  clio::run::u32 start_line_ = 0;
  clio::run::u32 end_line_ = 0;
  std::string summary_;

  template <class Archive>
  void serialize(Archive &ar) {
    ar(path_, score_, start_line_, end_line_, summary_);
  }
};

/** Scan + summarize + embed a directory. Blocks its worker while it runs. */
struct IndexPathTask : public clio::run::Task {
  IN clio::run::priv::string root_;
  IN clio::run::u32 max_files_;  // 0 = unlimited
  OUT clio::run::u32 files_;
  OUT clio::run::u32 chunks_;
  OUT clio::run::u32 summaries_;
  OUT clio::run::u32 summary_failures_;
  OUT clio::run::u32 embed_failures_;
  OUT float seconds_summarize_;
  OUT float seconds_embed_;

  // SHM constructor
  IndexPathTask()
      : clio::run::Task(),
        root_(CLIO_PRIV_ALLOC),
        max_files_(0),
        files_(0),
        chunks_(0),
        summaries_(0),
        summary_failures_(0),
        embed_failures_(0),
        seconds_summarize_(0.0f),
        seconds_embed_(0.0f) {}

  // Emplace constructor
  explicit IndexPathTask(const clio::run::TaskId &task_id,
                         const clio::run::PoolId &pool_id,
                         const clio::run::PoolQuery &pool_query,
                         const std::string &root, clio::run::u32 max_files)
      : clio::run::Task(task_id, pool_id, pool_query, Method::kIndexPath),
        root_(CLIO_PRIV_ALLOC, root),
        max_files_(max_files),
        files_(0),
        chunks_(0),
        summaries_(0),
        summary_failures_(0),
        embed_failures_(0),
        seconds_summarize_(0.0f),
        seconds_embed_(0.0f) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kIndexPath;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template <typename Archive>
  void SerializeIn(Archive &ar) {
    Task::SerializeIn(ar);
    ar(root_, max_files_);
  }

  template <typename Archive>
  void SerializeOut(Archive &ar) {
    Task::SerializeOut(ar);
    ar(files_, chunks_, summaries_, summary_failures_, embed_failures_,
       seconds_summarize_, seconds_embed_);
  }

  void Copy(const ctp::ipc::FullPtr<IndexPathTask> &other) {
    Task::Copy(other.template Cast<Task>());
    root_ = other->root_;
    max_files_ = other->max_files_;
    files_ = other->files_;
    chunks_ = other->chunks_;
    summaries_ = other->summaries_;
    summary_failures_ = other->summary_failures_;
    embed_failures_ = other->embed_failures_;
    seconds_summarize_ = other->seconds_summarize_;
    seconds_embed_ = other->seconds_embed_;
  }

  /** Each replica indexed its own node's path: sum the counters. */
  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task> &other_base) {
    Task::AggregateOut(other_base);
    auto other = other_base.template Cast<IndexPathTask>();
    files_ += other->files_;
    chunks_ += other->chunks_;
    summaries_ += other->summaries_;
    summary_failures_ += other->summary_failures_;
    embed_failures_ += other->embed_failures_;
    seconds_summarize_ += other->seconds_summarize_;
    seconds_embed_ += other->seconds_embed_;
  }
};

/** Rank indexed files for a query. Return code 2 = embedding failed. */
struct QueryTask : public clio::run::Task {
  IN clio::run::priv::string query_;
  IN clio::run::u32 k_;
  IN clio::run::u32 mode_;  // QueryMode::*
  OUT std::vector<HitRow> hits_;

  // SHM constructor
  QueryTask()
      : clio::run::Task(),
        query_(CLIO_PRIV_ALLOC),
        k_(5),
        mode_(QueryMode::kHybrid) {}

  // Emplace constructor
  explicit QueryTask(const clio::run::TaskId &task_id,
                     const clio::run::PoolId &pool_id,
                     const clio::run::PoolQuery &pool_query,
                     const std::string &query, clio::run::u32 k,
                     clio::run::u32 mode)
      : clio::run::Task(task_id, pool_id, pool_query, Method::kQuery),
        query_(CLIO_PRIV_ALLOC, query),
        k_(k),
        mode_(mode) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kQuery;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template <typename Archive>
  void SerializeIn(Archive &ar) {
    Task::SerializeIn(ar);
    ar(query_, k_, mode_);
  }

  template <typename Archive>
  void SerializeOut(Archive &ar) {
    Task::SerializeOut(ar);
    ar(hits_);
  }

  void Copy(const ctp::ipc::FullPtr<QueryTask> &other) {
    Task::Copy(other.template Cast<Task>());
    query_ = other->query_;
    k_ = other->k_;
    mode_ = other->mode_;
    hits_ = other->hits_;
  }

  /** Merge replicas' partial rankings; keep the global top-k by score. */
  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task> &other_base) {
    Task::AggregateOut(other_base);
    auto other = other_base.template Cast<QueryTask>();
    if (other->hits_.empty()) return;
    hits_.insert(hits_.end(), other->hits_.begin(), other->hits_.end());
    std::sort(hits_.begin(), hits_.end(),
              [](const HitRow &a, const HitRow &b) {
                return a.score_ > b.score_;
              });
    if (k_ > 0 && hits_.size() > k_) hits_.resize(k_);
  }
};

/** Index size. */
struct StatusTask : public clio::run::Task {
  OUT clio::run::u32 files_;
  OUT clio::run::u32 chunks_;

  // SHM constructor
  StatusTask() : clio::run::Task(), files_(0), chunks_(0) {}

  // Emplace constructor
  explicit StatusTask(const clio::run::TaskId &task_id,
                      const clio::run::PoolId &pool_id,
                      const clio::run::PoolQuery &pool_query)
      : clio::run::Task(task_id, pool_id, pool_query, Method::kStatus),
        files_(0),
        chunks_(0) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kStatus;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template <typename Archive>
  void SerializeIn(Archive &ar) {
    Task::SerializeIn(ar);
  }

  template <typename Archive>
  void SerializeOut(Archive &ar) {
    Task::SerializeOut(ar);
    ar(files_, chunks_);
  }

  void Copy(const ctp::ipc::FullPtr<StatusTask> &other) {
    Task::Copy(other.template Cast<Task>());
    files_ = other->files_;
    chunks_ = other->chunks_;
  }

  void AggregateOut(const ctp::ipc::FullPtr<clio::run::Task> &other_base) {
    Task::AggregateOut(other_base);
    auto other = other_base.template Cast<StatusTask>();
    files_ += other->files_;
    chunks_ += other->chunks_;
  }
};

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CODE_INDEXER_TASKS_H_

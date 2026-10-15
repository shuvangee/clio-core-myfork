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

#include <clio_cae/code_indexer/code_indexer_runtime.h>
#include <clio_cae/code_indexer/ollama_summarizer.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Define ChiMod entry points using the CLIO_TASK_CC macro.
CLIO_TASK_CC(clio::cae::code_indexer::Runtime)

namespace clio::cae::code_indexer {

namespace {

/** Return code QueryTask reports when a needed embedding call failed. */
constexpr int kEmbedFailed = 2;

CodeIndexerConfig ToPipelineConfig(const CreateParams &p) {
  CodeIndexerConfig c;
  c.chunk_lines = p.chunk_lines;
  c.max_chunk_chars = p.max_chunk_chars;
  c.max_chunks_per_file = p.max_chunks_per_file;
  c.truncate_dim = p.embed_dim;
  if (!p.query_prefix.empty()) c.query_prefix = p.query_prefix;
  if (!p.doc_prefix.empty()) c.doc_prefix = p.doc_prefix;
  if (!p.summarize_model.empty()) {
    c.summary_fingerprint = p.summarize_model + "|" + p.summarize_prompt + "|" +
                            std::to_string(p.summarize_ctx) + "|" +
                            std::to_string(p.summarize_num_predict);
  }
  return c;
}

std::shared_ptr<Embedder> MakeEmbedder(const CreateParams &p) {
  if (p.embed_endpoint.empty()) return nullptr;  // keyword-only pool
  EmbedConfig e;
  e.endpoint = p.embed_endpoint;
  e.model = p.embed_model;
  e.api = p.embed_api == "openai" ? EmbedApi::kOpenAI : EmbedApi::kOllama;
  e.batch_size = p.embed_batch;
  return std::make_shared<HttpEmbedder>(e);
}

SummarizeFn MakeSummarizer(const CreateParams &p) {
  if (p.summarize_model.empty() || p.summarize_endpoint.empty()) return nullptr;
  OllamaSummarizeConfig s;
  s.endpoint = p.summarize_endpoint;
  s.model = p.summarize_model;
  s.prompt = p.summarize_prompt;
  s.context_length = p.summarize_ctx;
  s.num_predict = p.summarize_num_predict;
  return MakeOllamaSummarizer(s);
}

SearchMode ToSearchMode(clio::run::u32 wire) {
  switch (wire) {
    case QueryMode::kSummary: return SearchMode::kSummary;
    case QueryMode::kCode: return SearchMode::kCode;
    case QueryMode::kKeyword: return SearchMode::kKeyword;
    case QueryMode::kKeywordSummary: return SearchMode::kKeywordSummary;
    case QueryMode::kKeywordGraph: return SearchMode::kKeywordGraph;
    default: return SearchMode::kHybrid;
  }
}

}  // namespace

clio::run::TaskResume Runtime::Create(clio::run::shared_ptr<CreateTask> &task) {
  CLIO_TASK_BODY_BEGIN
  params_ = task->GetParams();
  if (!params_.cte_pool_id.empty()) {
    size_t dot = params_.cte_pool_id.find('.');
    if (dot != std::string::npos) {
      clio::run::PoolId cte_pool(
          static_cast<clio::run::u32>(
              std::stoul(params_.cte_pool_id.substr(0, dot))),
          static_cast<clio::run::u32>(
              std::stoul(params_.cte_pool_id.substr(dot + 1))));
      cte_ = std::make_unique<clio::cte::core::Client>(cte_pool);
    } else {
      HLOG(kWarning, "CodeIndexer: bad cte_pool_id '{}', persistence off",
           params_.cte_pool_id);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    indexer_ = std::make_unique<CodeIndexer>(
        ToPipelineConfig(params_), MakeEmbedder(params_),
        MakeSummarizer(params_));
  }
  HLOG(kInfo,
       "CodeIndexer container created for pool: {} (ID: {}), embed='{}'@'{}' "
       "summarize='{}'",
       pool_name_, pool_id_, params_.embed_model, params_.embed_endpoint,
       params_.summarize_model);
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Destroy(
    clio::run::shared_ptr<DestroyTask> &task) {
  CLIO_TASK_BODY_BEGIN
  {
    std::lock_guard<std::mutex> lock(mu_);
    indexer_.reset();
  }
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

// LCOV_EXCL_START: admin Monitor hook, never dispatched by the unit suite.
clio::run::TaskResume Runtime::Monitor(
    clio::run::shared_ptr<MonitorTask> &task) {
  CLIO_TASK_BODY_BEGIN
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}
// LCOV_EXCL_STOP

// ---------------------------------------------------------------------------
// Persisted-summary cache (CTE tag)
// ---------------------------------------------------------------------------

clio::run::TaskResume Runtime::EnsureTag(bool *ok) {
  CLIO_TASK_BODY_BEGIN
  *ok = false;
  if (!cte_) CLIO_CO_RETURN;
  if (tag_ready_) {
    *ok = true;
    CLIO_CO_RETURN;
  }
  {
    auto fut = cte_->AsyncGetOrCreateTag(params_.cte_tag,
                                         clio::cte::core::TagId::GetNull(),
                                         clio::run::PoolQuery::Local());
    CLIO_CO_AWAIT(fut);
    if (fut->GetReturnCode() == 0) {
      tag_id_ = fut->tag_id_;
      tag_ready_ = true;
      *ok = true;
    } else {
      HLOG(kWarning, "CodeIndexer: cannot open CTE tag '{}' (rc={})",
           params_.cte_tag, fut->GetReturnCode());
    }
  }
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::ReadBlob(const std::string &name,
                                        std::string *out, bool *found) {
  CLIO_TASK_BODY_BEGIN
  *found = false;
  out->clear();
  {
    auto sz = cte_->AsyncGetBlobSize(tag_id_, name,
                                     clio::run::PoolQuery::Local());
    CLIO_CO_AWAIT(sz);
    size_t n = sz->GetReturnCode() == 0 ? static_cast<size_t>(sz->size_) : 0;
    if (n == 0) CLIO_CO_RETURN;
    std::string buf(n, '\0');
    auto get = cte_->AsyncGetBlob(tag_id_, name, 0, n, 0, &buf[0],
                                  clio::run::PoolQuery::Local());
    CLIO_CO_AWAIT(get);
    if (get->GetReturnCode() == 0) {
      *out = std::move(buf);
      *found = true;
    }
  }
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::WriteBlob(const std::string &name,
                                         const std::string &data, bool *ok) {
  CLIO_TASK_BODY_BEGIN
  *ok = false;
  if (data.empty()) CLIO_CO_RETURN;
  {
    auto put = cte_->AsyncPutBlob(tag_id_, name, 0, data.size(), data.data(),
                                  1.0f, clio::cte::core::Context(), 0,
                                  clio::run::PoolQuery::Local());
    CLIO_CO_AWAIT(put);
    *ok = put.get() != nullptr && put->GetReturnCode() == 0;
  }
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::LoadSummary(const std::string &path,
                                           const std::string &fingerprint,
                                           std::string *summary_out) {
  CLIO_TASK_BODY_BEGIN
  summary_out->clear();
  bool tag_ok = false;
  CLIO_CO_AWAIT(EnsureTag(&tag_ok));
  if (!tag_ok) CLIO_CO_RETURN;
  std::string stored_fp;
  bool found = false;
  CLIO_CO_AWAIT(ReadBlob(path + "#fp", &stored_fp, &found));
  if (!found || stored_fp != fingerprint) CLIO_CO_RETURN;
  CLIO_CO_AWAIT(ReadBlob(path + "#summary", summary_out, &found));
  if (!found) summary_out->clear();
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::StoreSummary(const std::string &path,
                                            const std::string &fingerprint,
                                            const std::string &summary,
                                            bool *ok) {
  CLIO_TASK_BODY_BEGIN
  *ok = false;
  bool tag_ok = false;
  CLIO_CO_AWAIT(EnsureTag(&tag_ok));
  if (!tag_ok) CLIO_CO_RETURN;
  // Summary first, fingerprint last: a crash between the two leaves a summary
  // with a stale/missing fingerprint, which reads as a miss, never as a hit
  // on the wrong text.
  bool put_summary = false;
  bool put_fp = false;
  CLIO_CO_AWAIT(WriteBlob(path + "#summary", summary, &put_summary));
  if (put_summary) CLIO_CO_AWAIT(WriteBlob(path + "#fp", fingerprint, &put_fp));
  *ok = put_summary && put_fp;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::IndexPath(
    clio::run::shared_ptr<IndexPathTask> &task) {
  CLIO_TASK_BODY_BEGIN
  ScanOptions scan;
  scan.max_files = task->max_files_;
  std::vector<SourceFile> files = ScanDirectory(task->root_.str(), scan);
  if (files.empty()) {
    HLOG(kWarning, "CodeIndexer: no source files under '{}'",
         task->root_.str());
    task->SetReturnCode(1);
    CLIO_CO_RETURN;
  }
  IndexStats total;
  size_t persist_failures = 0;
  // One file per lock acquisition so Query is not starved for the whole scan.
  // The lock is never held across a CTE await.
  for (const SourceFile &f : files) {
    std::string fingerprint;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!indexer_) break;
      fingerprint = indexer_->Fingerprint(f);
    }
    std::string cached;
    if (cte_) CLIO_CO_AWAIT(LoadSummary(f.path, fingerprint, &cached));

    IndexStats s;
    std::string summary;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!indexer_) break;
      s = indexer_->IndexFile(f, cached.empty() ? nullptr : &cached, &summary);
    }
    // Persist only what was freshly generated; a cache hit is already there.
    if (cte_ && s.summaries > 0 && !summary.empty()) {
      bool stored = false;
      CLIO_CO_AWAIT(StoreSummary(f.path, fingerprint, summary, &stored));
      if (!stored) ++persist_failures;
    }
    total.files += s.files;
    total.chunks += s.chunks;
    total.summaries += s.summaries;
    total.summary_failures += s.summary_failures;
    total.cache_hits += s.cache_hits;
    total.embed_failures += s.embed_failures;
    total.seconds_summarize += s.seconds_summarize;
    total.seconds_embed += s.seconds_embed;
  }
  task->files_ = static_cast<clio::run::u32>(total.files);
  task->chunks_ = static_cast<clio::run::u32>(total.chunks);
  task->summaries_ = static_cast<clio::run::u32>(total.summaries);
  task->summary_failures_ = static_cast<clio::run::u32>(total.summary_failures);
  task->cache_hits_ = static_cast<clio::run::u32>(total.cache_hits);
  task->persist_failures_ = static_cast<clio::run::u32>(persist_failures);
  task->embed_failures_ = static_cast<clio::run::u32>(total.embed_failures);
  task->seconds_summarize_ = static_cast<float>(total.seconds_summarize);
  task->seconds_embed_ = static_cast<float>(total.seconds_embed);
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Query(clio::run::shared_ptr<QueryTask> &task) {
  CLIO_TASK_BODY_BEGIN
  std::vector<SearchHit> hits;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (indexer_) {
      ok = indexer_->Query(task->query_.str(), task->k_,
                           ToSearchMode(task->mode_), &hits);
    }
  }
  task->hits_.clear();
  for (const SearchHit &h : hits) {
    HitRow r;
    r.path_ = h.path;
    r.score_ = h.score;
    r.start_line_ = static_cast<clio::run::u32>(h.start_line);
    r.end_line_ = static_cast<clio::run::u32>(h.end_line);
    r.summary_ = h.summary;
    task->hits_.push_back(std::move(r));
  }
  task->SetReturnCode(ok ? 0 : kEmbedFailed);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Status(clio::run::shared_ptr<StatusTask> &task) {
  CLIO_TASK_BODY_BEGIN
  {
    std::lock_guard<std::mutex> lock(mu_);
    task->files_ =
        indexer_ ? static_cast<clio::run::u32>(indexer_->NumFiles()) : 0;
    task->chunks_ =
        indexer_ ? static_cast<clio::run::u32>(indexer_->NumChunks()) : 0;
  }
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::u64 Runtime::GetWorkRemaining() const {
  // Indexing and queries are inline on the task; nothing is queued behind it.
  return 0;
}

}  // namespace clio::cae::code_indexer

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


#include <clio_cae/code_indexer/code_indexer.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>

namespace clio::cae::code_indexer {

namespace {

using Clock = std::chrono::steady_clock;

double Since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

/** Reciprocal-rank-fusion constant; 60 is the value from the original paper. */
constexpr double kRrfK = 60.0;

}  // namespace

std::string DefaultSummaryPrompt() {
  return "You are indexing a source file so a developer can find it later by "
         "describing what they need. Summarize the file below in 3-5 "
         "sentences: its purpose, the main types and functions it defines, "
         "and what it depends on. Use the identifiers that appear in the "
         "code. Do not quote code and do not add commentary.";
}

CodeIndexer::CodeIndexer(CodeIndexerConfig cfg,
                         std::shared_ptr<Embedder> embedder,
                         SummarizeFn summarize)
    : cfg_(std::move(cfg)),
      embedder_(std::move(embedder)),
      summarize_(std::move(summarize)) {}

std::string CodeIndexer::FormatDoc(const std::string &title,
                                   const std::string &text) const {
  std::string prefix = cfg_.doc_prefix;
  const std::string key = "{title}";
  size_t at = prefix.find(key);
  if (at != std::string::npos) prefix.replace(at, key.size(), title);
  return prefix + text;
}

bool CodeIndexer::EmbedOne(const std::string &text,
                           std::vector<float> *out) const {
  if (!embedder_) return false;
  std::vector<std::vector<float>> vecs;
  if (!embedder_->Embed({text}, &vecs) || vecs.size() != 1) return false;
  *out = std::move(vecs[0]);
  TruncateAndNormalize(out, cfg_.truncate_dim);
  return !out->empty();
}

IndexStats CodeIndexer::IndexFiles(const std::vector<SourceFile> &files) {
  IndexStats st;
  for (const SourceFile &f : files) {
    uint32_t fid = static_cast<uint32_t>(files_.size());
    FileEntry entry;
    entry.path = f.path;

    if (summarize_) {
      auto t0 = Clock::now();
      entry.summary = summarize_(f);
      st.seconds_summarize += Since(t0);
      if (entry.summary.empty()) {
        ++st.summary_failures;
      } else {
        ++st.summaries;
      }
    }

    std::vector<CodeChunk> chunks = ChunkLines(
        f.content, cfg_.chunk_lines, cfg_.max_chunk_chars,
        cfg_.max_chunks_per_file);

    // One Embed() call per file: a failure costs this file's vectors, never
    // another file's. Slot 0 is the summary when there is one.
    std::vector<std::string> texts;
    bool has_summary = !entry.summary.empty();
    if (has_summary) texts.push_back(FormatDoc(f.path, entry.summary));
    for (const CodeChunk &c : chunks) {
      texts.push_back(FormatDoc(
          f.path + ":" + std::to_string(c.start_line) + "-" +
              std::to_string(c.end_line),
          c.text));
    }

    std::vector<std::vector<float>> vecs;
    auto t1 = Clock::now();
    bool ok = !texts.empty() && embedder_ && embedder_->Embed(texts, &vecs) &&
              vecs.size() == texts.size();
    st.seconds_embed += Since(t1);

    if (ok) {
      size_t slot = 0;
      if (has_summary) {
        TruncateAndNormalize(&vecs[0], cfg_.truncate_dim);
        summary_index_.Add(fid, std::move(vecs[0]));
        slot = 1;
      }
      for (size_t i = 0; i < chunks.size(); ++i) {
        TruncateAndNormalize(&vecs[slot + i], cfg_.truncate_dim);
        uint32_t cid = static_cast<uint32_t>(chunks_.size());
        chunks_.push_back({fid, chunks[i].start_line, chunks[i].end_line});
        code_index_.Add(cid, std::move(vecs[slot + i]));
      }
    } else if (!texts.empty() && embedder_) {
      ++st.embed_failures;
    }

    keyword_index_.Add(fid, f.path + "\n" + f.content);
    st.chunks += chunks.size();
    ++st.files;
    files_.push_back(std::move(entry));
  }
  return st;
}

SearchHit CodeIndexer::MakeHit(uint32_t file, float score,
                               const ChunkRef *chunk) const {
  SearchHit h;
  h.path = files_[file].path;
  h.score = score;
  h.summary = files_[file].summary;
  if (chunk != nullptr) {
    h.start_line = chunk->start_line;
    h.end_line = chunk->end_line;
  }
  return h;
}

// Best chunk score per file, best file first. `best_chunk` (optional) maps a
// file id to the chunk index that won it.
std::vector<ScoredId> CodeIndexer::RankFilesByCode(
    const std::vector<float> &q, size_t limit,
    std::vector<size_t> *best_chunk) const {
  std::vector<ScoredId> per_chunk = code_index_.Search(q, code_index_.size());
  std::unordered_map<uint32_t, ScoredId> best;
  std::unordered_map<uint32_t, size_t> best_c;
  for (const ScoredId &s : per_chunk) {  // already best-first
    uint32_t fid = chunks_[s.id].file;
    if (best.find(fid) == best.end()) {
      best[fid] = {fid, s.score};
      best_c[fid] = s.id;
    }
  }
  std::vector<ScoredId> out;
  out.reserve(best.size());
  for (const auto &kv : best) out.push_back(kv.second);
  std::sort(out.begin(), out.end(), [](const ScoredId &a, const ScoredId &b) {
    return a.score != b.score ? a.score > b.score : a.id < b.id;
  });
  if (out.size() > limit) out.resize(limit);
  if (best_chunk != nullptr) {
    best_chunk->assign(files_.size(), static_cast<size_t>(-1));
    for (const auto &kv : best_c) (*best_chunk)[kv.first] = kv.second;
  }
  return out;
}

bool CodeIndexer::Query(const std::string &query, size_t k, SearchMode mode,
                        std::vector<SearchHit> *hits) const {
  hits->clear();
  if (k == 0 || files_.empty()) return true;

  if (mode == SearchMode::kKeyword) {
    for (const ScoredId &s : keyword_index_.Search(query, k)) {
      hits->push_back(MakeHit(s.id, s.score, nullptr));
    }
    return true;
  }

  std::vector<float> q;
  if (!EmbedOne(cfg_.query_prefix + query, &q)) return false;

  if (mode == SearchMode::kSummary) {
    for (const ScoredId &s : summary_index_.Search(q, k)) {
      hits->push_back(MakeHit(s.id, s.score, nullptr));
    }
    return true;
  }

  std::vector<size_t> best_chunk;
  if (mode == SearchMode::kCode) {
    for (const ScoredId &s : RankFilesByCode(q, k, &best_chunk)) {
      hits->push_back(MakeHit(s.id, s.score, &chunks_[best_chunk[s.id]]));
    }
    return true;
  }

  // kHybrid: fuse the two full rankings by reciprocal rank.
  std::unordered_map<uint32_t, double> fused;
  size_t rank = 0;
  for (const ScoredId &s : summary_index_.Search(q, files_.size())) {
    fused[s.id] += 1.0 / (kRrfK + static_cast<double>(++rank));
  }
  rank = 0;
  for (const ScoredId &s : RankFilesByCode(q, files_.size(), &best_chunk)) {
    fused[s.id] += 1.0 / (kRrfK + static_cast<double>(++rank));
  }
  std::vector<ScoredId> ranked;
  for (const auto &kv : fused) {
    ranked.push_back({kv.first, static_cast<float>(kv.second)});
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const ScoredId &a, const ScoredId &b) {
              return a.score != b.score ? a.score > b.score : a.id < b.id;
            });
  if (ranked.size() > k) ranked.resize(k);
  for (const ScoredId &s : ranked) {
    constexpr size_t kNone = static_cast<size_t>(-1);
    size_t c = s.id < best_chunk.size() ? best_chunk[s.id] : kNone;
    hits->push_back(MakeHit(s.id, s.score, c == kNone ? nullptr : &chunks_[c]));
  }
  return true;
}

std::string CodeIndexer::SummaryOf(const std::string &path) const {
  for (const FileEntry &f : files_) {
    if (f.path == path) return f.summary;
  }
  return "";
}

}  // namespace clio::cae::code_indexer

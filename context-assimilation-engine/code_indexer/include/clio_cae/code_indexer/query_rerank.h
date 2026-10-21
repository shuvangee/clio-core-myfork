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


#ifndef CLIO_CAE_CODE_INDEXER_QUERY_RERANK_H_
#define CLIO_CAE_CODE_INDEXER_QUERY_RERANK_H_

/**
 * Query-conditioned reranking with an evolving prompt.
 *
 * A static per-file summary is written before anyone knows what will be
 * asked. This asks the model about the file *given the current question*
 * ("is this the file that does X?") and reorders the top hits by its answer.
 * PromptEvolver is the "evolving" part: it remembers which earlier questions
 * were answered by which files and feeds that history back into the next
 * prompt, so the prompt adapts to what the agent has actually been asking.
 *
 * Model-free: the judge is injected, so tests use a deterministic fake.
 */

#include <clio_cae/code_indexer/code_indexer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace clio::cae::code_indexer {

/**
 * Relevance of `file` to `query`, 0 (no) .. 10 (exactly it); negative on
 * failure. `history` is PromptEvolver::History() text ("" when none).
 */
using JudgeFn = std::function<double(const std::string &query,
                                     const std::string &history,
                                     const SourceFile &file)>;

/** Remembers (question -> useful file) pairs and renders them as prompt text. */
class PromptEvolver {
 public:
  void RecordUseful(const std::string &query, const std::string &path) {
    entries_.push_back({query, path});
  }

  /** The most recent `max_entries` pairs as prompt text; "" when none/0. */
  std::string History(size_t max_entries) const {
    if (max_entries == 0 || entries_.empty()) return "";
    size_t from =
        entries_.size() > max_entries ? entries_.size() - max_entries : 0;
    std::string out =
        "Earlier questions in this session and the file that answered each:\n";
    for (size_t i = from; i < entries_.size(); ++i) {
      out += "- \"" + entries_[i].first + "\" -> " + entries_[i].second + "\n";
    }
    return out;
  }

  size_t size() const { return entries_.size(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

struct RerankStats {
  size_t judged = 0;
  /** Judge returned a negative score (counted as 0 in the ordering). */
  size_t failures = 0;
};

/**
 * Reorder the first `top_n` of `hits` by judge score (descending, ties keep
 * the original order); hits after `top_n` follow unchanged. A hit whose file
 * content cannot be found keeps its original score as no information.
 */
inline std::vector<SearchHit> RerankByJudge(
    const std::string &query, const std::vector<SearchHit> &hits, size_t top_n,
    const std::function<bool(const std::string &, SourceFile *)> &content_of,
    const JudgeFn &judge, const std::string &history, RerankStats *stats) {
  size_t n = std::min(top_n, hits.size());
  std::vector<std::pair<double, size_t>> scored;  // (score, original rank)
  for (size_t i = 0; i < n; ++i) {
    SourceFile f;
    double s = 0.0;
    if (content_of(hits[i].path, &f)) {
      s = judge(query, history, f);
      if (stats != nullptr) ++stats->judged;
      if (s < 0.0) {
        if (stats != nullptr) ++stats->failures;
        s = 0.0;
      }
    }
    scored.push_back({s, i});
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](const auto &a, const auto &b) { return a.first > b.first; });
  std::vector<SearchHit> out;
  for (const auto &p : scored) {
    SearchHit h = hits[p.second];
    h.score = static_cast<float>(p.first);
    out.push_back(std::move(h));
  }
  for (size_t i = n; i < hits.size(); ++i) out.push_back(hits[i]);
  return out;
}

/** A question-focused description of `file` ("" on failure). */
using FocusFn = std::function<std::string(const std::string &query,
                                          const std::string &history,
                                          const SourceFile &file)>;

/** Embeds one query string; empty vector on failure. */
using EmbedTextFn = std::function<std::vector<float>(const std::string &)>;

/** Embeds a document (title = file path, text = its description). */
using EmbedDocFn = std::function<std::vector<float>(const std::string &title,
                                                    const std::string &text)>;

/** Outcome of RerankByFocusedSummary(). */
struct FocusRerank {
  /** Candidates ordered by cosine(query, embedded focused summary) only. */
  std::vector<SearchHit> by_focus;
  /** norm(BM25) + min-max-normalized focus cosine, ties by BM25 rank. */
  std::vector<SearchHit> fused;
  size_t failures = 0;
};

inline float CosineUnit(const std::vector<float> &a, const std::vector<float> &b) {
  if (a.size() != b.size() || a.empty()) return 0.0f;
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  return (na > 0 && nb > 0) ? static_cast<float>(dot / std::sqrt(na * nb)) : 0.0f;
}

/**
 * Re-describe each of the first `top_n` hits *for this question*, embed that
 * description and compare it with the question's embedding. `hits[i].score`
 * must be the BM25 score. A failed focus/embed gives the file cosine 0 and is
 * counted. Hits after top_n are appended unchanged to both outputs.
 */
inline FocusRerank RerankByFocusedSummary(
    const std::string &query, const std::vector<SearchHit> &hits, size_t top_n,
    const std::function<bool(const std::string &, SourceFile *)> &content_of,
    const FocusFn &focus, const EmbedTextFn &embed_query,
    const EmbedDocFn &embed_doc, const std::string &history) {
  FocusRerank out;
  size_t n = std::min(top_n, hits.size());
  std::vector<float> q = embed_query(query);
  std::vector<float> cos(n, 0.0f);
  std::vector<std::string> fs(n);
  for (size_t i = 0; i < n; ++i) {
    SourceFile f;
    if (!content_of(hits[i].path, &f)) continue;
    fs[i] = focus(query, history, f);
    std::vector<float> d = fs[i].empty() ? std::vector<float>{}
                                         : embed_doc(hits[i].path, fs[i]);
    if (q.empty() || d.empty()) {
      ++out.failures;
      continue;
    }
    cos[i] = CosineUnit(q, d);
  }
  float lo = n ? *std::min_element(cos.begin(), cos.end()) : 0.0f;
  float hi = n ? *std::max_element(cos.begin(), cos.end()) : 0.0f;
  float mx = 0.0f;
  for (size_t i = 0; i < n; ++i) mx = std::max(mx, hits[i].score);
  std::vector<std::pair<double, size_t>> a, b;
  for (size_t i = 0; i < n; ++i) {
    double cn = hi > lo ? (cos[i] - lo) / (hi - lo) : 0.0;
    a.push_back({cos[i], i});
    b.push_back({(mx > 0 ? hits[i].score / mx : 0.0) + cn, i});
  }
  auto by_desc = [](const auto &x, const auto &y) { return x.first > y.first; };
  std::stable_sort(a.begin(), a.end(), by_desc);
  std::stable_sort(b.begin(), b.end(), by_desc);
  for (const auto &p : a) {
    SearchHit h = hits[p.second];
    h.score = static_cast<float>(p.first);
    h.summary = fs[p.second];
    out.by_focus.push_back(std::move(h));
  }
  for (const auto &p : b) {
    SearchHit h = hits[p.second];
    h.score = static_cast<float>(p.first);
    h.summary = fs[p.second];
    out.fused.push_back(std::move(h));
  }
  for (size_t i = n; i < hits.size(); ++i) {
    out.by_focus.push_back(hits[i]);
    out.fused.push_back(hits[i]);
  }
  return out;
}

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_QUERY_RERANK_H_

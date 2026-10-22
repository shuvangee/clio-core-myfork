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


#ifndef CLIO_CAE_CODE_INDEXER_TWO_PHASE_H_
#define CLIO_CAE_CODE_INDEXER_TWO_PHASE_H_

/**
 * Two-phase retrieval: find the right files cheaply, then prune inside them.
 *
 *   Phase 1  BM25 over path + summary + source (SearchMode::kKeywordSummary)
 *            returns the top `top_tags` files. In CTE terms a file is a TAG,
 *            a collection of blobs; phase 1 never looks at blobs.
 *   Phase 2  every blob (a line-window chunk) of each returned tag is shown
 *            to a model together with the question; blobs scoring below
 *            `keep_threshold` are dropped. A tag left with no blob is dropped.
 *   Result   the pruned set: only the kept blobs, grouped by tag.
 *
 * Besides the model score each blob carries its own BM25 score over the
 * candidate blobs, so a caller can compare the model's pruning with a plain
 * blob-level BM25 cut of the same size.
 */

#include <clio_cae/code_indexer/code_indexer.h>
#include <clio_cae/code_indexer/llm_roles.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace clio::cae::code_indexer {

struct TwoPhaseConfig {
  size_t top_tags = 3;
  /** Blobs with a judge score below this are pruned. */
  double keep_threshold = 6.0;
  /** MUST equal the CodeIndexerConfig the files were indexed with. */
  size_t chunk_lines = 60;
  size_t max_chunk_chars = 6000;
  size_t max_chunks_per_file = 24;
};

struct BlobResult {
  size_t start_line = 0;
  size_t end_line = 0;
  /** Model score 0..10; -1 when the model call failed (treated as pruned). */
  double judge = 0.0;
  /** BM25 score of this blob among all candidate blobs of this query. */
  double bm25 = 0.0;
  bool kept = false;
  std::string text;
};

struct TagResult {
  SearchHit tag;  // phase-1 hit (score = BM25 over path+summary+source)
  std::vector<BlobResult> blobs;
  size_t kept_blobs = 0;
};

struct TwoPhaseResult {
  std::vector<TagResult> tags;
  size_t judged = 0;
  size_t kept = 0;
  size_t failures = 0;
  /** Characters of all candidate blobs / of the kept blobs. */
  size_t chars_in = 0;
  size_t chars_out = 0;
};

inline TwoPhaseResult TwoPhaseQuery(
    const CodeIndexer &idx, const std::string &query, const TwoPhaseConfig &cfg,
    const std::function<bool(const std::string &, SourceFile *)> &content_of,
    const BlobJudgeFn &judge) {
  TwoPhaseResult out;
  std::vector<SearchHit> hits;
  idx.Query(query, cfg.top_tags, SearchMode::kKeywordSummary, &hits);

  // Collect candidate blobs first so BM25 can be computed over all of them.
  Bm25Index blob_bm25;
  std::vector<std::pair<size_t, size_t>> where;  // (tag, blob) per bm25 id
  std::vector<SourceFile> files(hits.size());
  for (size_t t = 0; t < hits.size(); ++t) {
    TagResult tr;
    tr.tag = hits[t];
    if (content_of(hits[t].path, &files[t])) {
      for (const CodeChunk &c : ChunkLines(files[t].content, cfg.chunk_lines,
                                           cfg.max_chunk_chars,
                                           cfg.max_chunks_per_file)) {
        BlobResult b;
        b.start_line = c.start_line;
        b.end_line = c.end_line;
        b.text = c.text;
        blob_bm25.Add(static_cast<uint32_t>(where.size()), c.text);
        where.push_back({t, tr.blobs.size()});
        out.chars_in += c.text.size();
        tr.blobs.push_back(std::move(b));
      }
    }
    out.tags.push_back(std::move(tr));
  }
  for (const ScoredId &s : blob_bm25.Search(query, where.size())) {
    out.tags[where[s.id].first].blobs[where[s.id].second].bm25 = s.score;
  }

  // Phase 2: the model looks at every blob of every returned tag.
  for (size_t t = 0; t < out.tags.size(); ++t) {
    TagResult &tr = out.tags[t];
    for (BlobResult &b : tr.blobs) {
      CodeChunk c;
      c.start_line = b.start_line;
      c.end_line = b.end_line;
      c.text = b.text;
      b.judge = judge(query, files[t], c);
      ++out.judged;
      if (b.judge < 0.0) {
        ++out.failures;
      } else if (b.judge >= cfg.keep_threshold) {
        b.kept = true;
        ++tr.kept_blobs;
        ++out.kept;
        out.chars_out += b.text.size();
      }
    }
  }
  return out;
}

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_TWO_PHASE_H_

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


#ifndef CLIO_CAE_CODE_INDEXER_CODE_INDEXER_H_
#define CLIO_CAE_CODE_INDEXER_CODE_INDEXER_H_

/**
 * The Code Indexer pipeline: summarize -> embed -> search, all in memory.
 *
 *   IndexFiles():  for every file
 *                    summary  = summarize(file)              (optional)
 *                    chunks   = line windows of the file
 *                    vectors  = embed(summary), embed(chunks)
 *   Query():       embed(query) -> rank files, in one of four modes:
 *                    kSummary  cosine over per-file summary vectors
 *                    kCode     best code-chunk cosine per file
 *                    kHybrid   reciprocal-rank fusion of the two
 *                    kKeyword  BM25 over path + source (no model needed)
 *
 * Summary and code retrieval are kept as separate modes on purpose: whether
 * an LLM summary is a better retrieval key than the code itself is an open
 * question for this module, so the structure has to be able to answer it.
 *
 * The model-facing parts are injected (Embedder, SummarizeFn), so the whole
 * pipeline runs under test with deterministic fakes and no inference server.
 * Everything lives in process memory; persistence and tiering through the CTE
 * are a later layer that wraps this class.
 */

#include <clio_cae/code_indexer/code_scanner.h>
#include <clio_cae/code_indexer/embed_client.h>
#include <clio_cae/code_indexer/vector_index.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace clio::cae::code_indexer {

/** Returns a file's summary, or "" on failure (the file is still indexed). */
using SummarizeFn = std::function<std::string(const SourceFile &)>;

/** How Query() ranks files. */
enum class SearchMode { kSummary, kCode, kHybrid, kKeyword };

/** Chunking, prefixes and truncation. Defaults suit EmbeddingGemma. */
struct CodeIndexerConfig {
  size_t chunk_lines = 60;
  size_t max_chunk_chars = 6000;
  size_t max_chunks_per_file = 24;
  /** Matryoshka dimension (768/512/256/128), 0 = native. */
  int truncate_dim = 0;
  /** Prepended to the query text before embedding. */
  std::string query_prefix = "task: code retrieval | query: ";
  /** Prepended to every document; "{title}" becomes the file path/range. */
  std::string doc_prefix = "title: {title} | text: ";
};

/** What an IndexFiles() call did. Counters are cumulative per call. */
struct IndexStats {
  size_t files = 0;
  size_t chunks = 0;
  size_t summaries = 0;
  size_t summary_failures = 0;
  size_t embed_failures = 0;
  double seconds_summarize = 0.0;
  double seconds_embed = 0.0;
};

/** One ranked file. start_line/end_line are 0 unless a chunk matched. */
struct SearchHit {
  std::string path;
  float score = 0.0f;
  std::string summary;
  size_t start_line = 0;
  size_t end_line = 0;
};

class CodeIndexer {
 public:
  /**
   * @param embedder Required for every mode except kKeyword.
   * @param summarize Optional; null indexes code only (no summaries).
   */
  CodeIndexer(CodeIndexerConfig cfg, std::shared_ptr<Embedder> embedder,
              SummarizeFn summarize);

  /** Index `files`, appending to whatever is already indexed. */
  IndexStats IndexFiles(const std::vector<SourceFile> &files);

  /**
   * Top-`k` files for `query`. Returns false when an embedding was needed and
   * failed (hits is then empty); a query that simply matches nothing is
   * true with no hits.
   */
  bool Query(const std::string &query, size_t k, SearchMode mode,
             std::vector<SearchHit> *hits) const;

  size_t NumFiles() const { return files_.size(); }
  size_t NumChunks() const { return chunks_.size(); }

  /** The stored summary of an indexed file ("" if none / unknown path). */
  std::string SummaryOf(const std::string &path) const;

 private:
  struct FileEntry {
    std::string path;
    std::string summary;
  };
  struct ChunkRef {
    uint32_t file = 0;
    size_t start_line = 0;
    size_t end_line = 0;
  };

  std::string FormatDoc(const std::string &title,
                        const std::string &text) const;
  bool EmbedOne(const std::string &text, std::vector<float> *out) const;
  SearchHit MakeHit(uint32_t file, float score, const ChunkRef *chunk) const;
  std::vector<ScoredId> RankFilesByCode(const std::vector<float> &q,
                                        size_t limit,
                                        std::vector<size_t> *best_chunk) const;

  CodeIndexerConfig cfg_;
  std::shared_ptr<Embedder> embedder_;
  SummarizeFn summarize_;
  std::vector<FileEntry> files_;
  std::vector<ChunkRef> chunks_;
  VectorIndex summary_index_;  // id = file index
  VectorIndex code_index_;     // id = chunk index
  Bm25Index keyword_index_;    // id = file index
};

/**
 * Build the default summarization prompt template. Kept as a function (not a
 * constant) so a deployment can later feed it evolved variants; what a good
 * prompt looks like is itself an open research question for this module.
 */
std::string DefaultSummaryPrompt();

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CODE_INDEXER_H_

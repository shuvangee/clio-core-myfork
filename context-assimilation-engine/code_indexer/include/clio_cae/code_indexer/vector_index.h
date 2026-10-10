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


#ifndef CLIO_CAE_CODE_INDEXER_VECTOR_INDEX_H_
#define CLIO_CAE_CODE_INDEXER_VECTOR_INDEX_H_

/**
 * The two ranking structures the Code Indexer searches with, both in memory:
 *
 *   - VectorIndex: exact (brute-force) cosine similarity over unit vectors.
 *     Exact is the right MVP choice: the target is small datasets that fit in
 *     memory, an ANN index would only add a recall knob to evaluate, and the
 *     class is the single seam to replace when scale matters.
 *   - Bm25Index: keyword baseline / fallback over identifier-aware tokens.
 *
 * Leaf header: standard library only.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace clio::cae::code_indexer {

/** One ranked result: caller-assigned item id and its score. */
struct ScoredId {
  uint32_t id = 0;
  float score = 0.0f;
};

/** Scale `v` to unit length in place. A zero vector is left untouched. */
inline void NormalizeL2(std::vector<float> *v) {
  double sq = 0.0;
  for (float x : *v) sq += static_cast<double>(x) * x;
  if (sq <= 0.0) return;
  float inv = static_cast<float>(1.0 / std::sqrt(sq));
  for (float &x : *v) x *= inv;
}

/**
 * Matryoshka truncation: keep the first `dim` components, then re-normalize
 * (the model card requires re-normalizing before cosine similarity).
 * dim <= 0 or >= size leaves the vector at its native size, normalized.
 */
inline void TruncateAndNormalize(std::vector<float> *v, int dim) {
  if (dim > 0 && static_cast<size_t>(dim) < v->size()) v->resize(dim);
  NormalizeL2(v);
}

/** Exact cosine-similarity index. Not thread-safe for concurrent Add. */
class VectorIndex {
 public:
  /**
   * Add a vector under `id`. The vector is normalized on insert. A vector
   * whose dimension differs from the first one added is rejected (false),
   * as is an empty vector.
   */
  bool Add(uint32_t id, std::vector<float> v) {
    if (v.empty()) return false;
    if (dim_ == 0) dim_ = v.size();
    if (v.size() != dim_) return false;
    NormalizeL2(&v);
    ids_.push_back(id);
    vecs_.push_back(std::move(v));
    return true;
  }

  size_t size() const { return ids_.size(); }
  size_t dim() const { return dim_; }

  /**
   * Top-`k` items by cosine similarity to `query`, best first. Ties break by
   * lower id, so results are deterministic. A query of the wrong dimension
   * returns nothing.
   */
  std::vector<ScoredId> Search(std::vector<float> query, size_t k) const {
    std::vector<ScoredId> out;
    if (query.size() != dim_ || dim_ == 0 || k == 0) return out;
    NormalizeL2(&query);
    out.reserve(ids_.size());
    for (size_t i = 0; i < ids_.size(); ++i) {
      float dot = 0.0f;
      const std::vector<float> &v = vecs_[i];
      for (size_t d = 0; d < dim_; ++d) dot += v[d] * query[d];
      out.push_back({ids_[i], dot});
    }
    std::sort(out.begin(), out.end(), [](const ScoredId &a, const ScoredId &b) {
      return a.score != b.score ? a.score > b.score : a.id < b.id;
    });
    if (out.size() > k) out.resize(k);
    return out;
  }

 private:
  size_t dim_ = 0;
  std::vector<uint32_t> ids_;
  std::vector<std::vector<float>> vecs_;
};

/**
 * Split text into lowercase search tokens, identifier-aware: "GetBlobSize"
 * and "get_blob_size" both become {get, blob, size}. Runs of digits are kept
 * as their own tokens. Single-character tokens are dropped.
 */
inline std::vector<std::string> Tokenize(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&]() {
    if (cur.size() > 1) out.push_back(cur);
    cur.clear();
  };
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (!std::isalnum(c)) {
      flush();
      continue;
    }
    bool upper = std::isupper(c) != 0;
    bool prev_lower =
        i > 0 && std::islower(static_cast<unsigned char>(text[i - 1]));
    bool next_lower = i + 1 < text.size() &&
                      std::islower(static_cast<unsigned char>(text[i + 1]));
    bool prev_upper =
        i > 0 && std::isupper(static_cast<unsigned char>(text[i - 1]));
    bool prev_digit =
        i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1]));
    bool digit = std::isdigit(c) != 0;
    // camelCase boundary (aB) or acronym end (ABc), and letter<->digit.
    if (upper && (prev_lower || (prev_upper && next_lower))) flush();
    if (digit != prev_digit && i > 0 && !cur.empty()) flush();
    cur.push_back(static_cast<char>(std::tolower(c)));
  }
  flush();
  return out;
}

/** Plain Okapi BM25 (k1=1.2, b=0.75) over Tokenize()d documents. */
class Bm25Index {
 public:
  void Add(uint32_t id, const std::string &text) {
    std::vector<std::string> toks = Tokenize(text);
    Doc d;
    d.id = id;
    d.len = toks.size();
    for (const std::string &t : toks) ++d.tf[t];
    for (const auto &kv : d.tf) ++df_[kv.first];
    total_len_ += d.len;
    docs_.push_back(std::move(d));
  }

  size_t size() const { return docs_.size(); }

  /** Top-`k` documents with a positive score for `query`, best first. */
  std::vector<ScoredId> Search(const std::string &query, size_t k) const {
    std::vector<ScoredId> out;
    if (docs_.empty() || k == 0) return out;
    std::vector<std::string> qt = Tokenize(query);
    double n = static_cast<double>(docs_.size());
    double avg = total_len_ / n;
    if (avg <= 0.0) return out;
    for (const Doc &d : docs_) {
      double s = 0.0;
      for (const std::string &t : qt) {
        auto tf_it = d.tf.find(t);
        if (tf_it == d.tf.end()) continue;
        double df = static_cast<double>(df_.at(t));
        double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
        double tf = tf_it->second;
        s += idf * tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * d.len / avg));
      }
      if (s > 0.0) out.push_back({d.id, static_cast<float>(s)});
    }
    std::sort(out.begin(), out.end(), [](const ScoredId &a, const ScoredId &b) {
      return a.score != b.score ? a.score > b.score : a.id < b.id;
    });
    if (out.size() > k) out.resize(k);
    return out;
  }

 private:
  struct Doc {
    uint32_t id = 0;
    size_t len = 0;
    std::unordered_map<std::string, uint32_t> tf;
  };
  std::vector<Doc> docs_;
  std::unordered_map<std::string, uint32_t> df_;
  double total_len_ = 0.0;
};

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_VECTOR_INDEX_H_

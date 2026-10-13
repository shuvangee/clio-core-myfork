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

#ifndef CLIO_CAE_CODE_INDEXER_EMBED_CACHE_H_
#define CLIO_CAE_CODE_INDEXER_EMBED_CACHE_H_

/**
 * A persistent embedding cache, as an Embedder decorator.
 *
 * Embedding every chunk of a large tree is the slow part of starting the
 * command-line indexer (tens of minutes for a repository). Wrapping the real
 * embedder in a CachingEmbedder makes every later run pay only for text it has
 * not seen: scan and chunking are deterministic, so an unchanged tree maps to
 * the same strings, and those hit the cache.
 *
 * The key is a 128-bit hash of (model name, exact input string). The exact
 * string includes the retrieval prefix, so changing the prefix or model never
 * returns a stale vector. Leaf header: standard library + embed_client.h.
 */

#include <clio_cae/code_indexer/embed_client.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace clio::cae::code_indexer {

class CachingEmbedder : public Embedder {
 public:
  /**
   * @param inner Embedder used for cache misses (required).
   * @param model_key Mixed into every key; use the embedding model name.
   * @param path Cache file. Loaded now if it exists and is well formed.
   */
  CachingEmbedder(std::shared_ptr<Embedder> inner, std::string model_key,
                  std::string path)
      : inner_(std::move(inner)),
        model_key_(std::move(model_key)),
        path_(std::move(path)) {
    Load();
  }

  bool Embed(const std::vector<std::string> &texts,
             std::vector<std::vector<float>> *out) override {
    out->clear();
    out->resize(texts.size());
    std::vector<size_t> miss_at;
    std::vector<std::string> miss_text;
    for (size_t i = 0; i < texts.size(); ++i) {
      auto it = map_.find(KeyOf(texts[i]));
      if (it != map_.end()) {
        (*out)[i] = it->second;
        ++hits_;
      } else {
        miss_at.push_back(i);
        miss_text.push_back(texts[i]);
      }
    }
    if (miss_text.empty()) return true;
    std::vector<std::vector<float>> fresh;
    if (!inner_ || !inner_->Embed(miss_text, &fresh) ||
        fresh.size() != miss_text.size()) {
      out->clear();
      return false;
    }
    for (size_t j = 0; j < miss_at.size(); ++j) {
      map_[KeyOf(miss_text[j])] = fresh[j];
      (*out)[miss_at[j]] = std::move(fresh[j]);
      ++misses_;
      dirty_ = true;
    }
    return true;
  }

  /** Write the cache to disk if it changed. Atomic (temp file + rename). */
  bool Save() {
    if (!dirty_ || path_.empty()) return true;
    std::string tmp = path_ + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return false;
      uint32_t dim = map_.empty() ? 0
                                  : static_cast<uint32_t>(
                                        map_.begin()->second.size());
      uint64_t n = map_.size();
      f.write(kMagic, 8);
      f.write(reinterpret_cast<const char *>(&dim), sizeof(dim));
      f.write(reinterpret_cast<const char *>(&n), sizeof(n));
      for (const auto &kv : map_) {
        if (kv.second.size() != dim) continue;  // defensive: skip odd sizes
        f.write(reinterpret_cast<const char *>(&kv.first.first), 8);
        f.write(reinterpret_cast<const char *>(&kv.first.second), 8);
        f.write(reinterpret_cast<const char *>(kv.second.data()),
                static_cast<std::streamsize>(dim * sizeof(float)));
      }
      if (!f) return false;
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) return false;
    dirty_ = false;
    return true;
  }

  size_t hits() const { return hits_; }
  size_t misses() const { return misses_; }
  size_t size() const { return map_.size(); }

 private:
  using Key = std::pair<uint64_t, uint64_t>;
  struct KeyHash {
    size_t operator()(const Key &k) const {
      return static_cast<size_t>(k.first ^ (k.second * 0x9E3779B97F4A7C15ULL));
    }
  };
  static constexpr char kMagic[9] = "CICACHE1";

  static uint64_t Fnv(const std::string &s, uint64_t seed) {
    uint64_t h = seed;
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    return h;
  }

  Key KeyOf(const std::string &text) const {
    std::string m = model_key_;
    m.push_back('\0');
    return {Fnv(text, Fnv(m, 1469598103934665603ULL)),
            Fnv(text, Fnv(m, 0xCBF29CE484222325ULL ^ 0x5851F42D4C957F2DULL))};
  }

  void Load() {
    if (path_.empty()) return;
    std::ifstream f(path_, std::ios::binary);
    if (!f) return;
    char magic[8];
    uint32_t dim = 0;
    uint64_t n = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char *>(&dim), sizeof(dim));
    f.read(reinterpret_cast<char *>(&n), sizeof(n));
    if (!f || std::string(magic, 8) != std::string(kMagic, 8) || dim == 0) {
      return;  // unreadable or foreign file: start empty
    }
    std::vector<float> v(dim);
    for (uint64_t i = 0; i < n; ++i) {
      Key k;
      f.read(reinterpret_cast<char *>(&k.first), 8);
      f.read(reinterpret_cast<char *>(&k.second), 8);
      f.read(reinterpret_cast<char *>(v.data()),
             static_cast<std::streamsize>(dim * sizeof(float)));
      if (!f) break;  // truncated file: keep what was read
      map_[k] = v;
    }
  }

  std::shared_ptr<Embedder> inner_;
  std::string model_key_;
  std::string path_;
  std::unordered_map<Key, std::vector<float>, KeyHash> map_;
  size_t hits_ = 0;
  size_t misses_ = 0;
  bool dirty_ = false;
};

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_EMBED_CACHE_H_

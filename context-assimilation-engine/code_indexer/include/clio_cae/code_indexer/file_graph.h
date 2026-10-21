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


#ifndef CLIO_CAE_CODE_INDEXER_FILE_GRAPH_H_
#define CLIO_CAE_CODE_INDEXER_FILE_GRAPH_H_

/**
 * File-relationship graph for the Code Indexer: which files are related to
 * which, derived from the source text alone (no model call).
 *
 * Two edge kinds, both undirected for retrieval purposes:
 *   - reference: file A includes/imports a path that resolves to file B
 *     (#include "x/y.h", import a.b, from a.b import c);
 *   - sibling: files that share a stem once test/impl decoration is removed
 *     (foo.h, foo.cc, test_foo.cc, foo_test.cc).
 *
 * Why this exists: a single-file summary cannot say that the file a developer
 * wants is the .cc implementing the header another hit names. The graph lets
 * a search result pull in its structural neighbours.
 *
 * Leaf header: standard library only.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace clio::cae::code_indexer {

/** Extract raw include/import targets from one source file's text. */
inline std::vector<std::string> ExtractReferences(const std::string &content) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    if (eol == std::string::npos) eol = content.size();
    std::string line = content.substr(pos, eol - pos);
    pos = eol + 1;
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) continue;
    if (line[i] == '#') {  // #include "a/b.h" or <a/b.h>
      size_t inc = line.find("include", i + 1);
      if (inc == std::string::npos) continue;
      size_t open = line.find_first_of("\"<", inc + 7);
      if (open == std::string::npos) continue;
      char close = line[open] == '"' ? '"' : '>';
      size_t end = line.find(close, open + 1);
      if (end == std::string::npos) continue;
      out.push_back(line.substr(open + 1, end - open - 1));
    } else if (line.compare(i, 7, "import ") == 0 ||
               line.compare(i, 5, "from ") == 0) {  // python
      size_t s = line.find(' ', i) + 1;
      size_t e = line.find_first_of(" ,", s);
      std::string mod = line.substr(s, e == std::string::npos ? e : e - s);
      if (mod.empty()) continue;
      for (char &c : mod) {
        if (c == '.') c = '/';
      }
      out.push_back(mod);  // resolved by suffix, with .py appended
    }
  }
  return out;
}

namespace detail {

inline std::string Stem(const std::string &path) {
  size_t slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base.resize(dot);
  if (base.rfind("test_", 0) == 0) base = base.substr(5);
  if (base.size() > 5 && base.compare(base.size() - 5, 5, "_test") == 0) {
    base.resize(base.size() - 5);
  }
  return base;
}

inline bool EndsWithPath(const std::string &path, const std::string &suffix) {
  if (suffix.empty() || path.size() < suffix.size()) return false;
  if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  return path.size() == suffix.size() ||
         path[path.size() - suffix.size() - 1] == '/';
}

}  // namespace detail

/** Immutable adjacency over file ids, built once from (path, refs) pairs. */
class FileGraph {
 public:
  struct Node {
    uint32_t id = 0;
    std::string path;
    std::vector<std::string> refs;  // from ExtractReferences()
  };

  /** Caps keep a popular header or a generic stem from linking everything. */
  static constexpr size_t kMaxResolveMatches = 3;
  static constexpr size_t kMaxSiblingGroup = 6;

  void Build(const std::vector<Node> &nodes) {
    adj_.clear();
    edges_ = 0;
    for (const Node &n : nodes) adj_[n.id];
    for (const Node &a : nodes) {
      for (const std::string &ref : a.refs) {
        std::vector<uint32_t> hits;
        for (const Node &b : nodes) {
          if (b.id == a.id) continue;
          if (detail::EndsWithPath(b.path, ref) ||
              detail::EndsWithPath(b.path, ref + ".py")) {
            hits.push_back(b.id);
          }
        }
        if (hits.size() > kMaxResolveMatches) continue;  // ambiguous
        for (uint32_t h : hits) Link(a.id, h);
      }
    }
    std::map<std::string, std::vector<uint32_t>> by_stem;
    for (const Node &n : nodes) by_stem[detail::Stem(n.path)].push_back(n.id);
    for (const auto &kv : by_stem) {
      const std::vector<uint32_t> &g = kv.second;
      if (g.size() < 2 || g.size() > kMaxSiblingGroup) continue;
      for (size_t i = 0; i < g.size(); ++i) {
        for (size_t j = i + 1; j < g.size(); ++j) Link(g[i], g[j]);
      }
    }
  }

  /** Neighbour ids of `id` (sorted), empty when unknown. */
  const std::vector<uint32_t> &Neighbors(uint32_t id) const {
    static const std::vector<uint32_t> kNone;
    auto it = adj_.find(id);
    return it == adj_.end() ? kNone : it->second;
  }

  /** Every undirected edge once, as (low id, high id), sorted. */
  std::vector<std::pair<uint32_t, uint32_t>> Edges() const {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto &kv : adj_) {
      for (uint32_t n : kv.second) {
        if (kv.first < n) out.push_back({kv.first, n});
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  size_t NumNodes() const { return adj_.size(); }
  size_t NumEdges() const { return edges_; }

 private:
  void Link(uint32_t a, uint32_t b) {
    std::vector<uint32_t> &la = adj_[a];
    if (std::find(la.begin(), la.end(), b) != la.end()) return;
    la.push_back(b);
    adj_[b].push_back(a);
    ++edges_;
    std::sort(la.begin(), la.end());
    std::sort(adj_[b].begin(), adj_[b].end());
  }

  std::unordered_map<uint32_t, std::vector<uint32_t>> adj_;
  size_t edges_ = 0;
};

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_FILE_GRAPH_H_

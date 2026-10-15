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


/**
 * clio_code_index: index a source tree and query it from the command line.
 *
 *   clio_code_index --root DIR --summ-model gemma3:1b \
 *       --embed-model embeddinggemma --query "where are blobs reorganized" \
 *       --mode all --top-k 5
 *
 * The index lives in this process only (MVP: everything in memory), so one
 * invocation indexes and then answers every --query / --queries-file entry.
 */

#include <clio_cae/code_indexer/code_indexer.h>
#include <clio_cae/code_indexer/embed_cache.h>
#include <clio_cae/code_indexer/ollama_summarizer.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace clio::cae::code_indexer;  // NOLINT: small CLI translation unit

namespace {

struct Args {
  std::string root;
  size_t max_files = 0;
  std::string embed_endpoint = "http://127.0.0.1:11434";
  std::string embed_model = "embeddinggemma";
  EmbedApi embed_api = EmbedApi::kOllama;
  int embed_dim = 0;
  size_t embed_batch = 16;
  bool no_embed = false;
  std::string cache_file;  // persistent embedding cache; empty = none
  std::string summ_endpoint = "http://127.0.0.1:11434";
  std::string summ_model;  // empty = no summaries
  int summ_ctx = 8192;
  int summ_num_predict = 256;
  std::vector<std::string> queries;
  std::string mode = "hybrid";
  size_t top_k = 5;
  bool show_summaries = false;
};

void Usage() {
  std::cerr
      << "usage: clio_code_index --root DIR [options]\n"
         "  --max-files N          index at most N files (sorted order)\n"
         "  --embed-endpoint URL   default http://127.0.0.1:11434\n"
         "  --embed-model NAME     default embeddinggemma\n"
         "  --embed-api ollama|openai   wire format (default ollama)\n"
         "  --embed-dim N          Matryoshka truncation (768/512/256/128)\n"
         "  --embed-batch N        inputs per request (default 16)\n"
         "  --no-embed             keyword (BM25) search only\n"
         "  --cache-file FILE      persistent embedding cache; later runs "
         "re-embed only new/changed text\n"
         "  --summ-endpoint URL    default http://127.0.0.1:11434\n"
         "  --summ-model NAME      omit to index code without summaries\n"
         "  --summ-ctx N           summarizer context tokens (default 8192)\n"
         "  --summ-num-predict N   cap per summary response (default 256)\n"
         "  --query TEXT           repeatable\n"
         "  --queries-file FILE    one query per line\n"
         "  --mode summary|code|hybrid|keyword|keyword+summary|keyword+graph|all   default hybrid\n"
         "  --top-k N              default 5\n"
         "  --show-summaries       print each hit's summary\n";
}

bool ParseArgs(int argc, char **argv, Args *a) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&]() -> const char * {
      return (i + 1 < argc) ? argv[++i] : nullptr;
    };
    if (k == "--no-embed") { a->no_embed = true; continue; }
    if (k == "--show-summaries") { a->show_summaries = true; continue; }
    const char *v = val();
    if (v == nullptr) {
      std::cerr << "missing value for " << k << "\n";
      return false;
    }
    if (k == "--root") a->root = v;
    else if (k == "--max-files") a->max_files = std::strtoul(v, nullptr, 10);
    else if (k == "--embed-endpoint") a->embed_endpoint = v;
    else if (k == "--embed-model") a->embed_model = v;
    else if (k == "--embed-api") {
      std::string s = v;
      if (s == "ollama") a->embed_api = EmbedApi::kOllama;
      else if (s == "openai") a->embed_api = EmbedApi::kOpenAI;
      else { std::cerr << "bad --embed-api\n"; return false; }
    }
    else if (k == "--cache-file") a->cache_file = v;
    else if (k == "--embed-dim") a->embed_dim = std::atoi(v);
    else if (k == "--embed-batch") {
      a->embed_batch = std::strtoul(v, nullptr, 10);
    }
    else if (k == "--summ-endpoint") a->summ_endpoint = v;
    else if (k == "--summ-model") a->summ_model = v;
    else if (k == "--summ-ctx") a->summ_ctx = std::atoi(v);
    else if (k == "--summ-num-predict") a->summ_num_predict = std::atoi(v);
    else if (k == "--query") a->queries.push_back(v);
    else if (k == "--queries-file") {
      std::ifstream in(v);
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '#') a->queries.push_back(line);
      }
    }
    else if (k == "--mode") a->mode = v;
    else if (k == "--top-k") a->top_k = std::strtoul(v, nullptr, 10);
    else { std::cerr << "unknown option " << k << "\n"; return false; }
  }
  return !a->root.empty();
}

const char *ModeName(SearchMode m) {
  switch (m) {
    case SearchMode::kSummary: return "summary";
    case SearchMode::kCode: return "code";
    case SearchMode::kHybrid: return "hybrid";
    case SearchMode::kKeyword: return "keyword";
    case SearchMode::kKeywordSummary: return "keyword+summary";
    case SearchMode::kKeywordGraph: return "keyword+graph";
  }
  return "?";
}

std::vector<SearchMode> ModesFor(const Args &a) {
  if (a.mode == "all") {
    std::vector<SearchMode> m = {SearchMode::kKeyword,
                                 SearchMode::kKeywordGraph};
    if (!a.no_embed) {
      m.push_back(SearchMode::kCode);
      if (!a.summ_model.empty()) {
        m.push_back(SearchMode::kSummary);
        m.push_back(SearchMode::kKeywordSummary);
        m.push_back(SearchMode::kHybrid);
      }
    }
    return m;
  }
  if (a.mode == "summary") return {SearchMode::kSummary};
  if (a.mode == "code") return {SearchMode::kCode};
  if (a.mode == "keyword") return {SearchMode::kKeyword};
  if (a.mode == "keyword+summary") return {SearchMode::kKeywordSummary};
  if (a.mode == "keyword+graph") return {SearchMode::kKeywordGraph};
  return {SearchMode::kHybrid};
}

void PrintHits(const std::vector<SearchHit> &hits, bool show_summaries) {
  size_t rank = 0;
  for (const SearchHit &h : hits) {
    std::cout << "  " << ++rank << ". " << h.path;
    if (h.start_line != 0) {
      std::cout << ":" << h.start_line << "-" << h.end_line;
    }
    std::cout << "  (" << h.score << ")\n";
    if (show_summaries && !h.summary.empty()) {
      std::cout << "       " << h.summary.substr(0, 400) << "\n";
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  Args a;
  if (!ParseArgs(argc, argv, &a)) {
    Usage();
    return 2;
  }

  ScanOptions scan;
  scan.max_files = a.max_files;
  std::vector<SourceFile> files = ScanDirectory(a.root, scan);
  std::cout << "scanned " << files.size() << " files under " << a.root << "\n";
  if (files.empty()) return 1;

  std::shared_ptr<Embedder> embedder;
  std::shared_ptr<CachingEmbedder> cache;
  if (!a.no_embed) {
    EmbedConfig ec;
    ec.endpoint = a.embed_endpoint;
    ec.model = a.embed_model;
    ec.api = a.embed_api;
    ec.batch_size = a.embed_batch;
    embedder = std::make_shared<HttpEmbedder>(ec);
    if (!a.cache_file.empty()) {
      cache = std::make_shared<CachingEmbedder>(embedder, a.embed_model,
                                                a.cache_file);
      embedder = cache;
    }
  }
  SummarizeFn summarize;
  if (!a.summ_model.empty()) {
    OllamaSummarizeConfig sc;
    sc.endpoint = a.summ_endpoint;
    sc.model = a.summ_model;
    sc.context_length = a.summ_ctx;
    sc.num_predict = a.summ_num_predict;
    summarize = MakeOllamaSummarizer(sc);
  }

  CodeIndexerConfig cfg;
  cfg.truncate_dim = a.embed_dim;
  CodeIndexer indexer(cfg, embedder, summarize);

  auto t0 = std::chrono::steady_clock::now();
  IndexStats st = indexer.IndexFiles(files);
  double total = std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - t0).count();
  std::cout << "indexed files=" << st.files << " chunks=" << st.chunks
            << " summaries=" << st.summaries
            << " summary_failures=" << st.summary_failures
            << " embed_failures=" << st.embed_failures
            << " | seconds: total=" << total
            << " summarize=" << st.seconds_summarize
            << " embed=" << st.seconds_embed << "\n";
  if (cache) {
    std::cout << "embedding cache: hits=" << cache->hits()
              << " misses=" << cache->misses()
              << (cache->Save() ? "" : " (SAVE FAILED)") << "\n";
  }

  int rc = 0;
  for (const std::string &q : a.queries) {
    std::cout << "\nQUERY: " << q << "\n";
    for (SearchMode m : ModesFor(a)) {
      std::vector<SearchHit> hits;
      bool ok = indexer.Query(q, a.top_k, m, &hits);
      std::cout << " [" << ModeName(m) << "]"
                << (ok ? "" : "  QUERY EMBED FAILED") << "\n";
      if (!ok) rc = 3;
      PrintHits(hits, a.show_summaries);
    }
  }
  return rc;
}

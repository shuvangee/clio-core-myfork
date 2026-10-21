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

#include <clio_cae/code_indexer/claude_client.h>
#include <clio_cae/code_indexer/code_indexer.h>
#include <clio_cae/code_indexer/embed_cache.h>
#include <clio_cae/code_indexer/ollama_summarizer.h>
#include <clio_cae/code_indexer/query_rerank.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
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
  std::string judge_model;     // empty = no query-conditioned rerank
  size_t judge_top = 10;
  size_t judge_history = 0;    // earlier (query -> file) pairs in the prompt
  std::string feedback_file;   // 'query<TAB>path[,path]' simulated feedback
  std::string verify_model;    // model that confirms/drops graph edges
  std::string focus_model;     // model that writes question-focused summaries
  size_t focus_top = 5;
  std::string backend = "ollama";  // ollama | claude (serves every model role)
  std::string claude_model;
  std::string claude_endpoint = "https://api.anthropic.com";
  int claude_max_tokens = 256;
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
         "  --mode summary|code|hybrid|keyword|keyword+summary|keyword+graph|keyword+graph-verified|all   default hybrid\n"
         "  --top-k N              default 5\n"
         "  --show-summaries       print each hit's summary\n"
         "  --judge-model NAME     also print [keyword+judge]: top hits of\n"
         "                         keyword reranked by a query-conditioned model\n"
         "  --judge-top N          hits to judge (default 10)\n"
         "  --judge-history N      earlier (query -> file) pairs fed to the\n"
         "                         prompt (evolving prompt, default 0)\n"
         "  --verify-model NAME    model that keeps/drops each file-graph edge\n"
         "                         (adds mode keyword+graph-verified)\n"
         "  --focus-model NAME     model that re-describes each top hit FOR the\n"
         "                         question; prints [keyword+focus] and\n"
         "                         [keyword+focus-fused] (needs the embedder)\n"
         "  --focus-top N          hits to re-describe (default 5)\n"
         "  --backend ollama|claude  which service answers every model role\n"
         "                         (a role is on when its *-model flag is set)\n"
         "  --claude-model ID      Claude model id (reads ANTHROPIC_API_KEY)\n"
         "  --claude-endpoint URL  default https://api.anthropic.com\n"
         "  --claude-max-tokens N  default 256\n"
         "  --feedback-file FILE   'query<TAB>path[,path]' lines: after each\n"
         "                         query the first path is recorded as the\n"
         "                         file that answered it (simulated feedback)\n";
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
    else if (k == "--verify-model") a->verify_model = v;
    else if (k == "--focus-model") a->focus_model = v;
    else if (k == "--focus-top") a->focus_top = std::strtoul(v, nullptr, 10);
    else if (k == "--backend") a->backend = v;
    else if (k == "--claude-model") a->claude_model = v;
    else if (k == "--claude-endpoint") a->claude_endpoint = v;
    else if (k == "--claude-max-tokens") a->claude_max_tokens = std::atoi(v);
    else if (k == "--judge-model") a->judge_model = v;
    else if (k == "--judge-top") a->judge_top = std::strtoul(v, nullptr, 10);
    else if (k == "--judge-history") {
      a->judge_history = std::strtoul(v, nullptr, 10);
    }
    else if (k == "--feedback-file") a->feedback_file = v;
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
    case SearchMode::kKeywordGraphVerified: return "keyword+graph-verified";
  }
  return "?";
}

std::vector<SearchMode> ModesFor(const Args &a) {
  if (a.mode == "all") {
    std::vector<SearchMode> m = {SearchMode::kKeyword,
                                 SearchMode::kKeywordGraph};
    if (!a.verify_model.empty()) m.push_back(SearchMode::kKeywordGraphVerified);
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
  if (a.mode == "keyword+graph-verified") {
    return {SearchMode::kKeywordGraphVerified};
  }
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
  const bool use_claude = a.backend == "claude";
  if (a.backend != "ollama" && !use_claude) {
    std::cerr << "bad --backend (ollama|claude)\n";
    return 2;
  }
  std::shared_ptr<ClaudeClient> claude;
  if (use_claude) {
    if (a.claude_model.empty()) {
      std::cerr << "--backend claude needs --claude-model\n";
      return 2;
    }
    ClaudeConfig cc;
    cc.endpoint = a.claude_endpoint;
    cc.model = a.claude_model;
    cc.max_tokens = a.claude_max_tokens;
    claude = std::make_shared<ClaudeClient>(cc);
    if (!claude->HasKey()) {
      std::cerr << "ANTHROPIC_API_KEY is not set in this environment\n";
      return 2;
    }
  }
  // One LlmFn per role (Ollama roles differ in how many tokens they may emit).
  auto ollama_llm = [&](const std::string &model, int num_predict) {
    OllamaSummarizeConfig oc;
    oc.endpoint = a.summ_endpoint;
    oc.model = model;
    oc.context_length = a.summ_ctx;
    oc.num_predict = num_predict;
    return MakeOllamaLlm(oc);
  };
  auto llm_for = [&](const std::string &model, int num_predict) -> LlmFn {
    return use_claude ? MakeClaudeLlm(claude) : ollama_llm(model, num_predict);
  };

  SummarizeFn summarize;
  if (!a.summ_model.empty()) {
    if (use_claude) {
      summarize = MakeSummarizerFromLlm(llm_for(a.summ_model, 0));
    } else {
      OllamaSummarizeConfig sc;
      sc.endpoint = a.summ_endpoint;
      sc.model = a.summ_model;
      sc.context_length = a.summ_ctx;
      sc.num_predict = a.summ_num_predict;
      summarize = MakeOllamaSummarizer(sc);
    }
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

  // Shared helpers for the model-backed extras below.
  std::unordered_map<std::string, const SourceFile *> by_path;
  for (const SourceFile &f : files) by_path[f.path] = &f;
  auto content_of = [&](const std::string &p, SourceFile *out) {
    auto it = by_path.find(p);
    if (it == by_path.end()) return false;
    *out = *it->second;
    return true;
  };

  // File-graph edge verification: the model keeps or drops each structural
  // edge; mode keyword+graph-verified then uses only the kept edges.
  if (!a.verify_model.empty()) {
    auto t1 = std::chrono::steady_clock::now();
    EdgeVerifyFn verify =
        MakeEdgeVerifierFromLlm(llm_for(a.verify_model, 8));
    auto r = indexer.VerifyGraphEdges(verify, content_of);
    std::cout << "graph verify: edges=" << indexer.NumGraphEdges()
              << " asked=" << r.first << " dropped=" << r.second
              << " seconds="
              << std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - t1).count()
              << "\n";
  }

  // Query-conditioned judge / focused-summary rerank (evolving prompt).
  // Simulated feedback: after each query the file named in --feedback-file is
  // recorded as the one that answered it, so later prompts carry that history.
  JudgeFn judge;
  FocusFn focus;
  PromptEvolver evolver;
  std::unordered_map<std::string, std::string> feedback;
  if (!a.judge_model.empty()) {
    if (use_claude) {
      judge = MakeJudgeFromLlm(llm_for(a.judge_model, 8));
    } else {
      OllamaSummarizeConfig jc;
      jc.endpoint = a.summ_endpoint;
      jc.model = a.judge_model;
      jc.context_length = a.summ_ctx;
      jc.num_predict = 8;
      judge = MakeOllamaJudge(jc);
    }
  }
  if (!a.focus_model.empty()) {
    if (!embedder) {
      std::cerr << "--focus-model needs the embedder (drop --no-embed)\n";
      return 2;
    }
    focus = MakeFocusFromLlm(llm_for(a.focus_model, 160));
  }
  if (!a.feedback_file.empty()) {
    std::ifstream fb(a.feedback_file);
    std::string line;
    while (std::getline(fb, line)) {
      size_t t = line.find('\t');
      if (t == std::string::npos || line[0] == '#') continue;
      std::string path = line.substr(t + 1);
      path = path.substr(0, path.find_first_of(",\t"));
      feedback[line.substr(0, t)] = path;
    }
  }
  auto embed_text = [&](const std::string &text) -> std::vector<float> {
    std::vector<std::vector<float>> v;
    if (!embedder || !embedder->Embed({text}, &v) || v.size() != 1) return {};
    TruncateAndNormalize(&v[0], cfg.truncate_dim);
    return v[0];
  };
  auto embed_query = [&](const std::string &q) {
    return embed_text(cfg.query_prefix + q);
  };
  auto embed_doc = [&](const std::string &title, const std::string &text) {
    std::string prefix = cfg.doc_prefix;
    size_t at = prefix.find("{title}");
    if (at != std::string::npos) prefix.replace(at, 7, title);
    return embed_text(prefix + text);
  };

  int rc = 0;
  for (const std::string &q : a.queries) {
    std::cout << "\nQUERY: " << q << "\n";
    const std::string history = evolver.History(a.judge_history);
    if (judge) {
      std::vector<SearchHit> base;
      indexer.Query(q, a.judge_top, SearchMode::kKeyword, &base);
      RerankStats rs;
      std::vector<SearchHit> rr = RerankByJudge(
          q, base, a.judge_top, content_of, judge, history, &rs);
      std::cout << " [keyword+judge]  judged=" << rs.judged
                << " failures=" << rs.failures << "\n";
      if (rr.size() > a.top_k) rr.resize(a.top_k);
      PrintHits(rr, false);
    }
    if (focus) {
      std::vector<SearchHit> base;
      indexer.Query(q, a.focus_top, SearchMode::kKeyword, &base);
      FocusRerank fr = RerankByFocusedSummary(
          q, base, a.focus_top, content_of, focus, embed_query,
          embed_doc, history);
      std::cout << " [keyword+focus]  failures=" << fr.failures << "\n";
      if (fr.by_focus.size() > a.top_k) fr.by_focus.resize(a.top_k);
      PrintHits(fr.by_focus, a.show_summaries);
      std::cout << " [keyword+focus-fused]\n";
      if (fr.fused.size() > a.top_k) fr.fused.resize(a.top_k);
      PrintHits(fr.fused, false);
    }
    auto fb = feedback.find(q);
    if (fb != feedback.end()) evolver.RecordUseful(q, fb->second);
    for (SearchMode m : ModesFor(a)) {
      std::vector<SearchHit> hits;
      bool ok = indexer.Query(q, a.top_k, m, &hits);
      std::cout << " [" << ModeName(m) << "]"
                << (ok ? "" : "  QUERY EMBED FAILED") << "\n";
      if (!ok) rc = 3;
      PrintHits(hits, a.show_summaries);
    }
  }
  if (claude) {
    ClaudeUsage u = claude->Usage();
    std::cout << "\nclaude usage: calls=" << u.calls
              << " failures=" << u.failures
              << " input_tokens=" << u.input_tokens
              << " output_tokens=" << u.output_tokens << "\n";
  }
  return rc;
}

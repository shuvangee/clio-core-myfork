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
 * clio_code_index_client: talk to a running Code Indexer pool.
 *
 *   clio_code_index_client index PATH [--max-files N]
 *   clio_code_index_client query "text" [--mode summary|code|hybrid|keyword]
 *                                       [--top-k N] [--show-summaries]
 *   clio_code_index_client status
 *
 * Global option: --pool MAJOR.MINOR (default 402.0). Needs a runtime started
 * with a compose entry for `clio_cae_code_indexer`; see README.md. PATH is
 * resolved on the node running the pool, not on the client.
 */

#include <clio_cae/code_indexer/code_indexer_client.h>
#include <clio_runtime/clio_runtime.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace ci = clio::cae::code_indexer;

namespace {

void Usage() {
  std::cerr
      << "usage: clio_code_index_client [--pool MAJOR.MINOR] <command>\n"
         "  index PATH [--max-files N]\n"
         "  query TEXT [--mode summary|code|hybrid|keyword] [--top-k N] "
         "[--show-summaries]\n"
         "  status\n";
}

bool ParsePool(const std::string &s, clio::run::PoolId *out) {
  size_t dot = s.find('.');
  if (dot == std::string::npos) return false;
  *out = clio::run::PoolId(
      static_cast<clio::run::u32>(std::strtoul(s.substr(0, dot).c_str(),
                                               nullptr, 10)),
      static_cast<clio::run::u32>(std::strtoul(s.substr(dot + 1).c_str(),
                                               nullptr, 10)));
  return true;
}

clio::run::u32 ModeFromName(const std::string &m, bool *ok) {
  *ok = true;
  if (m == "summary") return ci::QueryMode::kSummary;
  if (m == "code") return ci::QueryMode::kCode;
  if (m == "hybrid") return ci::QueryMode::kHybrid;
  if (m == "keyword") return ci::QueryMode::kKeyword;
  *ok = false;
  return ci::QueryMode::kHybrid;
}

int RunIndex(ci::Client *c, const std::string &path, clio::run::u32 max_files) {
  auto t = c->AsyncIndexPath(path, max_files);
  t.Wait();
  if (t->GetReturnCode() != 0) {
    std::cerr << "index failed (rc=" << t->GetReturnCode()
              << "); is PATH readable on the pool's node and non-empty?\n";
    return 1;
  }
  std::cout << "indexed files=" << t->files_ << " chunks=" << t->chunks_
            << " summaries=" << t->summaries_
            << " summary_failures=" << t->summary_failures_
            << " cache_hits=" << t->cache_hits_
            << " persist_failures=" << t->persist_failures_
            << " embed_failures=" << t->embed_failures_
            << " | seconds: summarize=" << t->seconds_summarize_
            << " embed=" << t->seconds_embed_ << "\n";
  return 0;
}

int RunQuery(ci::Client *c, const std::string &text, clio::run::u32 mode,
             clio::run::u32 k, bool show_summaries) {
  auto t = c->AsyncQuery(text, k, mode);
  t.Wait();
  int rc = t->GetReturnCode();
  if (rc != 0) {
    std::cerr << (rc == 2 ? "embedding the query failed (is the embedding "
                            "server up?)\n"
                          : "query failed\n");
    return 3;
  }
  size_t rank = 0;
  for (const ci::HitRow &h : t->hits_) {
    std::cout << "  " << ++rank << ". " << h.path_;
    if (h.start_line_ != 0) {
      std::cout << ":" << h.start_line_ << "-" << h.end_line_;
    }
    std::cout << "  (" << h.score_ << ")\n";
    if (show_summaries && !h.summary_.empty()) {
      std::cout << "       " << h.summary_.substr(0, 400) << "\n";
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  clio::run::PoolId pool = ci::kCodeIndexerPoolId;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--pool" && i + 1 < argc) {
      if (!ParsePool(argv[++i], &pool)) {
        Usage();
        return 2;
      }
    } else {
      args.push_back(a);
    }
  }
  if (args.empty()) {
    Usage();
    return 2;
  }

  std::string cmd = args[0];
  std::string text;          // path or query
  clio::run::u32 max_files = 0, k = 5, mode = ci::QueryMode::kHybrid;
  bool show_summaries = false;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string &a = args[i];
    bool has_val = i + 1 < args.size();
    if (a == "--max-files" && has_val) {
      max_files = std::strtoul(args[++i].c_str(), nullptr, 10);
    } else if (a == "--top-k" && has_val) {
      k = std::strtoul(args[++i].c_str(), nullptr, 10);
    } else if (a == "--mode" && has_val) {
      bool ok = false;
      mode = ModeFromName(args[++i], &ok);
      if (!ok) {
        Usage();
        return 2;
      }
    } else if (a == "--show-summaries") {
      show_summaries = true;
    } else if (text.empty()) {
      text = a;
    } else {
      Usage();
      return 2;
    }
  }
  if ((cmd == "index" || cmd == "query") && text.empty()) {
    Usage();
    return 2;
  }
  if (cmd != "index" && cmd != "query" && cmd != "status") {
    Usage();
    return 2;
  }

  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, false)) {
    std::cerr << "could not connect to the CLIO runtime; is it running?\n";
    return 1;
  }
  ci::Client client(pool);
  if (cmd == "index") return RunIndex(&client, text, max_files);
  if (cmd == "query") return RunQuery(&client, text, mode, k, show_summaries);
  auto st = client.AsyncStatus();
  st.Wait();
  std::cout << "files=" << st->files_ << " chunks=" << st->chunks_ << "\n";
  return st->GetReturnCode() == 0 ? 0 : 1;
}

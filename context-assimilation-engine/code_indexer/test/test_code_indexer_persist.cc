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
 * Summary persistence through a live runtime: IndexPath writes each generated
 * summary into a CTE tag, and a later IndexPath over the same files reads them
 * back instead of calling the summarizer. Models are in-process HTTP stubs, so
 * this verifies the persistence plumbing (blob layout, fingerprint check,
 * hit/miss accounting), not retrieval quality.
 */

#include "simple_test.h"
#include "summarizer_http_stub.h"

#include <clio_cae/code_indexer/code_indexer_client.h>
#include <clio_cae/code_indexer/cte_ingest.h>
#include <clio_cte/core/core_client.h>
#include <clio_ctp/introspect/system_info.h>
#include <clio_runtime/clio_runtime.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using clio_cae_test::OneShotHttpServer;
namespace ci = clio::cae::code_indexer;

namespace {

constexpr unsigned short kEmbedPort = 21436;
constexpr unsigned short kSummarizePort = 21437;
constexpr const char *kSummary = "A persisted stub summary.";

void WriteFile(const fs::path &p, const std::string &content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

/** Read a whole CTE blob as text; "" if absent. */
std::string ReadBlob(clio::cte::core::Client *cte,
                     const clio::cte::core::TagId &tag,
                     const std::string &name) {
  auto sz = cte->AsyncGetBlobSize(tag, name);
  sz.Wait();
  if (sz->GetReturnCode() != 0 || sz->size_ == 0) return std::string();
  std::string out(sz->size_, '\0');
  auto get = cte->AsyncGetBlob(tag, name, 0, out.size(), 0, &out[0]);
  get.Wait();
  return get->GetReturnCode() == 0 ? out : std::string();
}

}  // namespace

TEST_CASE("Summaries persist to the CTE and are reused on re-index",
          "[cae][code_indexer][persist]") {
  OneShotHttpServer embed("HTTP/1.1 200 OK\r\n",
                          "{\"embeddings\":[[0.1,0.2,0.3]]}", kEmbedPort);
  OneShotHttpServer summarize(
      "HTTP/1.1 200 OK\r\n",
      std::string("{\"response\":\"") + kSummary + "\"}", kSummarizePort);
  REQUIRE(embed.Port() == kEmbedPort);
  REQUIRE(summarize.Port() == kSummarizePort);

  fs::path dir = fs::temp_directory_path() /
                 ("clio_ci_persist_" +
                  std::to_string(std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count()));
  WriteFile(dir / "a.cc", "int FirstFile;\n");
  WriteFile(dir / "sub/b.cc", "int SecondFile;\n");

  fs::path config_path =
      fs::path(__FILE__).parent_path() / "test_code_indexer_persist.yaml";
  ctp::SystemInfo::Setenv("CLIO_SERVER_CONF", config_path.string(), 1);
  REQUIRE(clio::run::CLIO_INIT(clio::run::RuntimeMode::kServer));
  SimpleTest::g_test_finalize = clio::run::CLIO_RUNTIME_FINALIZE;
  std::this_thread::sleep_for(1s);

  ci::Client client(ci::kCodeIndexerPoolId);
  // Files live in the CTE (Clio FS); the indexer never reads the disk.
  REQUIRE(ci::IngestTree(dir.string(), "/ci_persist").files == 2);

  // 1. First index: every summary is generated and persisted.
  auto first = client.AsyncIndexPath("/ci_persist");
  first.Wait();
  REQUIRE(first->GetReturnCode() == 0);
  REQUIRE(first->files_ == 2);
  REQUIRE(first->summaries_ == 2);
  REQUIRE(first->cache_hits_ == 0);
  REQUIRE(first->persist_failures_ == 0);
  size_t generate_calls = summarize.RequestCount();
  REQUIRE(generate_calls == 2);

  // 2. The CTE holds the text and the fingerprint under the documented names.
  clio::cte::core::Client cte(clio::run::PoolId(513, 0));
  auto tag = cte.AsyncGetOrCreateTag("ci_summaries");
  tag.Wait();
  REQUIRE(tag->GetReturnCode() == 0);
  REQUIRE(ReadBlob(&cte, tag->tag_id_, "a.cc#summary") == kSummary);
  REQUIRE(ReadBlob(&cte, tag->tag_id_, "sub/b.cc#summary") == kSummary);
  REQUIRE(!ReadBlob(&cte, tag->tag_id_, "a.cc#fp").empty());

  // 3. Re-index unchanged files: all hits, summarizer not called again, and
  //    the index does not grow (re-indexing replaces).
  auto second = client.AsyncIndexPath("/ci_persist");
  second.Wait();
  REQUIRE(second->GetReturnCode() == 0);
  REQUIRE(second->cache_hits_ == 2);
  REQUIRE(second->summaries_ == 0);
  REQUIRE(summarize.RequestCount() == generate_calls);
  auto st = client.AsyncStatus();
  st.Wait();
  REQUIRE(st->files_ == 2);

  // 4. Edit one file: that file misses (new fingerprint), the other hits.
  WriteFile(dir / "a.cc", "int FirstFile; int Changed;\n");
  REQUIRE(ci::IngestTree(dir.string(), "/ci_persist").files == 2);  // re-put
  auto third = client.AsyncIndexPath("/ci_persist");
  third.Wait();
  REQUIRE(third->cache_hits_ == 1);
  REQUIRE(third->summaries_ == 1);
  REQUIRE(summarize.RequestCount() == generate_calls + 1);

  // 5. The summaries are what queries return.
  auto q = client.AsyncQuery("FirstFile", 5, ci::QueryMode::kKeyword);
  q.Wait();
  REQUIRE(q->hits_.size() >= 1);
  REQUIRE(q->hits_[0].summary_ == kSummary);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

SIMPLE_TEST_MAIN()

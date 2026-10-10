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
 * The Code Indexer pool through a live runtime: IndexPath -> Status -> Query.
 * The embedding model is an in-process HTTP stub, so this checks the pool's
 * plumbing (task serialization, container state, return codes), not retrieval
 * quality, which is covered by test_code_indexer and the live evaluation.
 */

#include "simple_test.h"
#include "summarizer_http_stub.h"

#include <clio_cae/code_indexer/code_indexer_client.h>
#include <clio_ctp/introspect/system_info.h>
#include <clio_runtime/clio_runtime.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using clio_cae_test::OneShotHttpServer;
namespace ci = clio::cae::code_indexer;

namespace {

constexpr unsigned short kStubPort = 21435;

void WriteFile(const fs::path &p, const std::string &content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

}  // namespace

TEST_CASE("Code Indexer pool indexes a directory and answers queries",
          "[cae][code_indexer][pool]") {
  // The stub answers every embed request with one 3-d vector.
  auto stub = std::make_unique<OneShotHttpServer>(
      "HTTP/1.1 200 OK\r\n", "{\"embeddings\":[[0.1,0.2,0.3]]}", kStubPort);
  REQUIRE(stub->Port() == kStubPort);

  fs::path dir = fs::temp_directory_path() /
                 ("clio_ci_pool_" +
                  std::to_string(std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count()));
  WriteFile(dir / "tiering/reorganize.cc",
            "void ReorganizeBlob(Blob b) { PromoteToFastTier(b); }\n");
  WriteFile(dir / "net/http_server.cc",
            "class HttpServer { void AcceptLoop(); };\n");
  WriteFile(dir / "README.md", "not source\n");

  fs::path config_path =
      fs::path(__FILE__).parent_path() / "test_code_indexer_pool.yaml";
  ctp::SystemInfo::Setenv("CLIO_SERVER_CONF", config_path.string(), 1);

  REQUIRE(clio::run::CLIO_INIT(clio::run::RuntimeMode::kServer));
  SimpleTest::g_test_finalize = clio::run::CLIO_RUNTIME_FINALIZE;
  std::this_thread::sleep_for(1s);

  ci::Client client(ci::kCodeIndexerPoolId);

  // 1. Empty index.
  auto st0 = client.AsyncStatus();
  st0.Wait();
  REQUIRE(st0->GetReturnCode() == 0);
  REQUIRE(st0->files_ == 0);

  // 2. Index the directory: two source files, README skipped.
  auto idx = client.AsyncIndexPath(dir.string());
  idx.Wait();
  REQUIRE(idx->GetReturnCode() == 0);
  REQUIRE(idx->files_ == 2);
  REQUIRE(idx->chunks_ == 2);
  REQUIRE(idx->embed_failures_ == 0);
  REQUIRE(idx->summaries_ == 0);  // no summarize_model configured

  auto st1 = client.AsyncStatus();
  st1.Wait();
  REQUIRE(st1->files_ == 2);
  REQUIRE(st1->chunks_ == 2);

  // 3. Keyword search needs no model and must find the right file.
  auto kw = client.AsyncQuery("ReorganizeBlob promote", 5, ci::QueryMode::kKeyword);
  kw.Wait();
  REQUIRE(kw->GetReturnCode() == 0);
  REQUIRE(kw->hits_.size() == 1);
  REQUIRE(kw->hits_[0].path_ == "tiering/reorganize.cc");

  // 4. Code search embeds the query through the stub; every vector is equal
  //    so both files come back (ranking is not asserted here).
  auto code = client.AsyncQuery("anything", 5, ci::QueryMode::kCode);
  code.Wait();
  REQUIRE(code->GetReturnCode() == 0);
  REQUIRE(code->hits_.size() == 2);
  REQUIRE(code->hits_[0].start_line_ == 1);

  // 5. An unindexed path reports failure instead of an empty success.
  auto bad = client.AsyncIndexPath((dir / "does_not_exist").string());
  bad.Wait();
  REQUIRE(bad->GetReturnCode() != 0);

  // 6. With the embedding server gone, an embedding query reports rc 2 and
  //    returns nothing; keyword search keeps working.
  stub->Stop();
  auto down = client.AsyncQuery("anything", 5, ci::QueryMode::kCode);
  down.Wait();
  REQUIRE(down->GetReturnCode() == 2);
  REQUIRE(down->hits_.empty());
  auto kw2 = client.AsyncQuery("HttpServer", 5, ci::QueryMode::kKeyword);
  kw2.Wait();
  REQUIRE(kw2->GetReturnCode() == 0);
  REQUIRE(kw2->hits_.size() == 1);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

SIMPLE_TEST_MAIN()

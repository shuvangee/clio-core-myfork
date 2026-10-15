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
 * Unit tests for the Code Indexer. Every model-facing piece is exercised
 * against either a deterministic fake or the in-process HTTP stub, so no
 * inference server is needed.
 */

#include "simple_test.h"
#include "summarizer_http_stub.h"

#include <clio_cae/code_indexer/code_indexer.h>
#include <clio_cae/code_indexer/embed_cache.h>
#include <clio_cae/code_indexer/ollama_summarizer.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace clio::cae::code_indexer;  // NOLINT
using clio_cae_test::OneShotHttpServer;

namespace {

namespace fs = std::filesystem;

/** Deterministic embedder: hashed bag of identifier tokens, 256 dims. */
class BagOfWordsEmbedder : public Embedder {
 public:
  bool fail = false;
  std::vector<std::string> seen;

  bool Embed(const std::vector<std::string> &texts,
             std::vector<std::vector<float>> *out) override {
    out->clear();
    if (fail) return false;
    for (const std::string &t : texts) {
      seen.push_back(t);
      std::vector<float> v(256, 0.0f);
      for (const std::string &tok : Tokenize(t)) {
        v[std::hash<std::string>{}(tok) % 256] += 1.0f;
      }
      out->push_back(std::move(v));
    }
    return true;
  }
};

/** A scratch directory removed on scope exit. */
struct TempDir {
  fs::path path;
  TempDir() {
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("clio_ci_test_" + std::to_string(ticks));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  void Write(const std::string &rel, const std::string &content) const {
    fs::path p = path / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
  }
};

std::vector<SourceFile> SampleFiles() {
  return {
      {"tiering/reorganize.cc",
       "// Moves a blob between storage tiers.\n"
       "void ReorganizeBlob(Blob b) { PromoteToFastTier(b); }\n"},
      {"net/http_server.cc",
       "// Accepts connections on a listening socket.\n"
       "class HttpServer { void AcceptLoop(); };\n"},
      {"ml/train.py",
       "def train_model():\n    optimizer.step()  # gradient descent\n"},
  };
}

}  // namespace

// ---------------------------------------------------------------- tokenizing

TEST_CASE("Tokenize splits camelCase, snake_case and digits",
          "[code_indexer][tokenize]") {
  // The trailing "2" is a single-character token and is dropped.
  std::vector<std::string> t =
      Tokenize("GetBlobSize get_blob_size HTTPServer2");
  std::vector<std::string> want = {"get",  "blob", "size", "get",
                                   "blob", "size", "http", "server"};
  REQUIRE(t == want);
}

// -------------------------------------------------------------- vector index

TEST_CASE("VectorIndex ranks by cosine and breaks ties by id",
          "[code_indexer][vector]") {
  VectorIndex idx;
  REQUIRE(idx.Add(7, {1.0f, 0.0f}));
  REQUIRE(idx.Add(3, {0.0f, 1.0f}));
  REQUIRE(idx.Add(5, {1.0f, 1.0f}));
  std::vector<ScoredId> r = idx.Search({1.0f, 0.1f}, 3);
  REQUIRE(r.size() == 3);
  REQUIRE(r[0].id == 7);
  REQUIRE(r[1].id == 5);
  REQUIRE(r[2].id == 3);
  // equal scores -> lower id first
  VectorIndex tie;
  tie.Add(9, {1.0f, 0.0f});
  tie.Add(2, {1.0f, 0.0f});
  REQUIRE(tie.Search({1.0f, 0.0f}, 2)[0].id == 2);
}

TEST_CASE("VectorIndex rejects empty and mismatched dimensions",
          "[code_indexer][vector]") {
  VectorIndex idx;
  REQUIRE_FALSE(idx.Add(0, {}));
  REQUIRE(idx.Add(1, {1.0f, 2.0f}));
  REQUIRE_FALSE(idx.Add(2, {1.0f, 2.0f, 3.0f}));
  REQUIRE(idx.size() == 1);
  REQUIRE(idx.Search({1.0f, 2.0f, 3.0f}, 3).empty());
}

TEST_CASE("TruncateAndNormalize keeps a prefix and re-normalizes",
          "[code_indexer][vector]") {
  std::vector<float> v = {3.0f, 4.0f, 12.0f};
  TruncateAndNormalize(&v, 2);
  REQUIRE(v.size() == 2);
  REQUIRE(std::fabs(v[0] - 0.6f) < 1e-6);
  REQUIRE(std::fabs(v[1] - 0.8f) < 1e-6);
  std::vector<float> native = {3.0f, 4.0f};
  TruncateAndNormalize(&native, 0);
  REQUIRE(native.size() == 2);
  REQUIRE(std::fabs(native[0] - 0.6f) < 1e-6);
}

TEST_CASE("Bm25Index ranks the document that matches rare terms",
          "[code_indexer][bm25]") {
  Bm25Index idx;
  idx.Add(0, "void ReorganizeBlob promote tier");
  idx.Add(1, "class HttpServer accept socket");
  idx.Add(2, "def train_model optimizer");
  std::vector<ScoredId> r = idx.Search("reorganize blob", 3);
  REQUIRE(r.size() == 1);
  REQUIRE(r[0].id == 0);
  REQUIRE(idx.Search("nothing matches here xyzzy", 3).empty());
}

// ------------------------------------------------------------------ chunking

TEST_CASE("ChunkLines windows by line count with 1-based ranges",
          "[code_indexer][chunk]") {
  std::string src;
  for (int i = 1; i <= 5; ++i) src += "line" + std::to_string(i) + "\n";
  std::vector<CodeChunk> c = ChunkLines(src, 2, 1000, 100);
  REQUIRE(c.size() == 3);
  REQUIRE(c[0].start_line == 1);
  REQUIRE(c[0].end_line == 2);
  REQUIRE(c[1].start_line == 3);
  REQUIRE(c[2].start_line == 5);
  REQUIRE(c[2].end_line == 5);
  REQUIRE(c[2].text == "line5\n");
}

TEST_CASE("ChunkLines honours the character and chunk-count limits",
          "[code_indexer][chunk]") {
  // 10-char lines, 25-char budget => at most 2 lines per chunk
  std::string src;
  for (int i = 0; i < 6; ++i) src += "123456789\n";
  std::vector<CodeChunk> c = ChunkLines(src, 100, 25, 100);
  REQUIRE(c.size() == 3);
  for (const CodeChunk &ch : c) REQUIRE(ch.text.size() <= 25);
  // chunk cap
  REQUIRE(ChunkLines(src, 100, 25, 2).size() == 2);
  // one over-long line is hard-cut
  std::vector<CodeChunk> longline =
      ChunkLines(std::string(50, 'x'), 10, 20, 100);
  REQUIRE(longline.size() == 3);
  for (const CodeChunk &ch : longline) REQUIRE(ch.text.size() <= 20);
  REQUIRE(ChunkLines("", 10, 100, 10).empty());
}

// ------------------------------------------------------------------- scanner

TEST_CASE("ScanDirectory filters by extension, ignored dirs, size and binary",
          "[code_indexer][scan]") {
  TempDir d;
  d.Write("b.cc", "int b;\n");
  d.Write("a.py", "a = 1\n");
  d.Write("sub/c.h", "// c\n");
  d.Write("README.md", "docs\n");
  d.Write("build/gen.cc", "int gen;\n");
  d.Write("external/lib.cc", "int lib;\n");
  d.Write("blob.cc", std::string("int x;\0\0\0", 9));
  d.Write("big.cc", std::string(2000, 'x'));

  ScanOptions o;
  o.max_file_bytes = 1000;
  std::vector<SourceFile> f = ScanDirectory(d.path.string(), o);
  std::vector<std::string> names;
  for (const SourceFile &s : f) names.push_back(s.path);
  std::vector<std::string> want = {"a.py", "b.cc", "sub/c.h"};
  REQUIRE(names == want);
  REQUIRE(f[1].content == "int b;\n");

  o.max_files = 2;
  REQUIRE(ScanDirectory(d.path.string(), o).size() == 2);
  REQUIRE(ScanDirectory((d.path / "missing").string(), o).empty());
}

// --------------------------------------------------------- embed wire format

TEST_CASE("ParseEmbedResponse reads the Ollama and OpenAI shapes",
          "[code_indexer][embed]") {
  std::vector<std::vector<float>> v;
  REQUIRE(ParseEmbedResponse(EmbedApi::kOllama,
                             "{\"embeddings\":[[1,2],[3,4]]}", 2, &v));
  REQUIRE(v.size() == 2);
  REQUIRE(v[1][0] == 3.0f);

  // OpenAI replies may be out of order; `index` decides placement.
  REQUIRE(ParseEmbedResponse(
      EmbedApi::kOpenAI,
      "{\"data\":[{\"index\":1,\"embedding\":[3,4]},"
      "{\"index\":0,\"embedding\":[1,2]}]}",
      2, &v));
  REQUIRE(v[0][0] == 1.0f);
  REQUIRE(v[1][0] == 3.0f);
}

TEST_CASE("ParseEmbedResponse rejects malformed replies",
          "[code_indexer][embed]") {
  std::vector<std::vector<float>> v;
  REQUIRE_FALSE(ParseEmbedResponse(EmbedApi::kOllama, "{not json", 1, &v));
  REQUIRE_FALSE(ParseEmbedResponse(EmbedApi::kOllama, "{}", 1, &v));
  // wrong count
  REQUIRE_FALSE(
      ParseEmbedResponse(EmbedApi::kOllama, "{\"embeddings\":[[1]]}", 2, &v));
  // unequal dims
  REQUIRE_FALSE(ParseEmbedResponse(
      EmbedApi::kOllama, "{\"embeddings\":[[1,2],[3]]}", 2, &v));
  // empty vector
  REQUIRE_FALSE(
      ParseEmbedResponse(EmbedApi::kOllama, "{\"embeddings\":[[]]}", 1, &v));
  // duplicate index
  REQUIRE_FALSE(ParseEmbedResponse(
      EmbedApi::kOpenAI,
      "{\"data\":[{\"index\":0,\"embedding\":[1]},"
      "{\"index\":0,\"embedding\":[2]}]}",
      2, &v));
  REQUIRE(v.empty());
}

TEST_CASE("HttpEmbedder talks to an Ollama-style endpoint",
          "[code_indexer][embed][http]") {
  OneShotHttpServer stub("HTTP/1.1 200 OK\r\n",
                         "{\"embeddings\":[[0.5,0.5],[1,0]]}");
  EmbedConfig c;
  c.endpoint = stub.Endpoint();
  c.model = "embeddinggemma";
  HttpEmbedder e(c);
  std::vector<std::vector<float>> out;
  REQUIRE(e.Embed({"a", "b"}, &out));
  REQUIRE(out.size() == 2);
  REQUIRE(stub.RequestCount() == 1);
  std::string body = stub.RequestBodies()[0];
  REQUIRE(body.find("\"model\":\"embeddinggemma\"") != std::string::npos);
  REQUIRE(body.find("\"input\":[\"a\",\"b\"]") != std::string::npos);
  stub.Stop();
}

TEST_CASE("HttpEmbedder splits work into batches",
          "[code_indexer][embed][http]") {
  OneShotHttpServer stub("HTTP/1.1 200 OK\r\n", "{\"embeddings\":[[1,2]]}");
  EmbedConfig c;
  c.endpoint = stub.Endpoint();
  c.batch_size = 1;
  HttpEmbedder e(c);
  std::vector<std::vector<float>> out;
  REQUIRE(e.Embed({"a", "b", "c"}, &out));
  REQUIRE(out.size() == 3);
  REQUIRE(stub.RequestCount() == 3);
  stub.Stop();
}

TEST_CASE("HttpEmbedder fails closed on HTTP errors and bad bodies",
          "[code_indexer][embed][http]") {
  std::vector<std::vector<float>> out;
  {
    OneShotHttpServer stub("HTTP/1.1 500 Internal Server Error\r\n", "{}");
    EmbedConfig c;
    c.endpoint = stub.Endpoint();
    HttpEmbedder e(c);
    REQUIRE_FALSE(e.Embed({"a"}, &out));
    REQUIRE(out.empty());
    stub.Stop();
  }
  {
    // The stub answers 1 embedding for a 2-input batch: a count mismatch.
    OneShotHttpServer stub("HTTP/1.1 200 OK\r\n", "{\"embeddings\":[[1]]}");
    EmbedConfig c;
    c.endpoint = stub.Endpoint();
    HttpEmbedder e(c);
    REQUIRE_FALSE(e.Embed({"a", "b"}, &out));
    stub.Stop();
  }
  {
    EmbedConfig c;
    c.endpoint = "http://127.0.0.1:1";
    HttpEmbedder e(c);
    REQUIRE_FALSE(e.Embed({"a"}, &out));
  }
}

// ----------------------------------------------------------------- pipeline

TEST_CASE("CodeIndexer code mode finds the file by what its code says",
          "[code_indexer][pipeline]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  CodeIndexer idx(CodeIndexerConfig{}, emb, nullptr);
  IndexStats st = idx.IndexFiles(SampleFiles());
  REQUIRE(st.files == 3);
  REQUIRE(st.chunks == 3);
  REQUIRE(st.embed_failures == 0);

  std::vector<SearchHit> hits;
  REQUIRE(
      idx.Query("reorganize blob promote tier", 2, SearchMode::kCode, &hits));
  REQUIRE(hits.size() == 2);
  REQUIRE(hits[0].path == "tiering/reorganize.cc");
  REQUIRE(hits[0].start_line == 1);
  REQUIRE(hits[0].end_line == 2);
}

TEST_CASE("CodeIndexer summary mode matches on the summary, not the code",
          "[code_indexer][pipeline]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  SummarizeFn fake = [](const SourceFile &f) -> std::string {
    if (f.path == "net/http_server.cc") return "Serves network requests.";
    return "Something else entirely.";
  };
  CodeIndexer idx(CodeIndexerConfig{}, emb, fake);
  IndexStats st = idx.IndexFiles(SampleFiles());
  REQUIRE(st.summaries == 3);

  std::vector<SearchHit> hits;
  // "network requests" never appears in the code of any file.
  REQUIRE(idx.Query("network requests", 1, SearchMode::kSummary, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].path == "net/http_server.cc");
  REQUIRE(hits[0].summary == "Serves network requests.");
  REQUIRE(idx.SummaryOf("net/http_server.cc") == "Serves network requests.");
}

TEST_CASE("CodeIndexer hybrid mode fuses summary and code rankings",
          "[code_indexer][pipeline]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  SummarizeFn fake = [](const SourceFile &f) -> std::string {
    return f.path == "ml/train.py" ? "gradient descent training loop" : "misc";
  };
  CodeIndexer idx(CodeIndexerConfig{}, emb, fake);
  idx.IndexFiles(SampleFiles());
  std::vector<SearchHit> hits;
  REQUIRE(
      idx.Query("optimizer gradient training", 3, SearchMode::kHybrid, &hits));
  REQUIRE(hits.size() == 3);
  REQUIRE(hits[0].path == "ml/train.py");
  REQUIRE(hits[0].start_line == 1);  // best chunk still reported
}

TEST_CASE("CodeIndexer keeps a file when its summary fails",
          "[code_indexer][pipeline]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  SummarizeFn failing = [](const SourceFile &) { return std::string(); };
  CodeIndexer idx(CodeIndexerConfig{}, emb, failing);
  IndexStats st = idx.IndexFiles(SampleFiles());
  REQUIRE(st.summary_failures == 3);
  REQUIRE(st.summaries == 0);
  std::vector<SearchHit> hits;
  REQUIRE(idx.Query("socket accept connections", 1, SearchMode::kCode, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].path == "net/http_server.cc");
  // no summary vectors exist, so summary mode finds nothing (not an error)
  REQUIRE(idx.Query("socket", 1, SearchMode::kSummary, &hits));
  REQUIRE(hits.empty());
}

TEST_CASE("CodeIndexer applies prefixes and Matryoshka truncation",
          "[code_indexer][pipeline]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  CodeIndexerConfig cfg;
  cfg.doc_prefix = "DOC[{title}]|";
  cfg.query_prefix = "Q|";
  cfg.truncate_dim = 64;
  CodeIndexer idx(cfg, emb, nullptr);
  idx.IndexFiles({{"x/y.cc", "int value;\n"}});
  REQUIRE(emb->seen.size() == 1);
  REQUIRE(emb->seen[0] == "DOC[x/y.cc:1-1]|int value;\n");
  std::vector<SearchHit> hits;
  REQUIRE(idx.Query("value", 1, SearchMode::kCode, &hits));
  REQUIRE(emb->seen.back() == "Q|value");
  REQUIRE(hits.size() == 1);
}

TEST_CASE("CodeIndexer reports embedding failures instead of hiding them",
          "[code_indexer][pipeline]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  CodeIndexer idx(CodeIndexerConfig{}, emb, nullptr);
  emb->fail = true;
  IndexStats st = idx.IndexFiles(SampleFiles());
  REQUIRE(st.embed_failures == 3);
  REQUIRE(idx.NumFiles() == 3);   // still keyword-searchable
  std::vector<SearchHit> hits;
  REQUIRE_FALSE(idx.Query("blob", 3, SearchMode::kCode, &hits));
  REQUIRE(hits.empty());
  REQUIRE(idx.Query("blob", 3, SearchMode::kKeyword, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].path == "tiering/reorganize.cc");
}

TEST_CASE("CodeIndexer keyword mode needs no embedder",
          "[code_indexer][pipeline]") {
  CodeIndexer idx(CodeIndexerConfig{}, nullptr, nullptr);
  IndexStats st = idx.IndexFiles(SampleFiles());
  REQUIRE(st.files == 3);
  REQUIRE(st.embed_failures == 0);
  std::vector<SearchHit> hits;
  REQUIRE(idx.Query("HttpServer", 1, SearchMode::kKeyword, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].path == "net/http_server.cc");
}

// ------------------------------------------------- re-indexing and caching

TEST_CASE("VectorIndex and Bm25Index support Erase",
          "[code_indexer][erase]") {
  VectorIndex v;
  v.Add(1, {1.0f, 0.0f});
  v.Add(2, {0.0f, 1.0f});
  REQUIRE(v.Erase(1));
  REQUIRE_FALSE(v.Erase(1));
  REQUIRE(v.size() == 1);
  REQUIRE(v.Search({1.0f, 0.0f}, 5)[0].id == 2);

  Bm25Index b;
  b.Add(1, "alpha beta");
  b.Add(2, "alpha gamma");
  REQUIRE(b.Erase(1));
  REQUIRE_FALSE(b.Erase(1));
  std::vector<ScoredId> r = b.Search("alpha", 5);
  REQUIRE(r.size() == 1);
  REQUIRE(r[0].id == 2);
  REQUIRE(b.Search("beta", 5).empty());
}

TEST_CASE("Re-indexing a path replaces the old entry",
          "[code_indexer][reindex]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  CodeIndexer idx(CodeIndexerConfig{}, emb, nullptr);
  idx.IndexFile({"a.cc", "int StaleGadget;\n"}, nullptr, nullptr);
  idx.IndexFile({"b.cc", "int Other;\n"}, nullptr, nullptr);
  REQUIRE(idx.NumFiles() == 2);
  idx.IndexFile({"a.cc", "int FreshWidget;\n"}, nullptr, nullptr);
  REQUIRE(idx.NumFiles() == 2);
  REQUIRE(idx.NumChunks() == 2);

  std::vector<SearchHit> hits;
  REQUIRE(idx.Query("StaleGadget", 5, SearchMode::kKeyword, &hits));
  REQUIRE(hits.empty());
  REQUIRE(idx.Query("FreshWidget", 5, SearchMode::kKeyword, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(idx.Query("FreshWidget", 5, SearchMode::kCode, &hits));
  REQUIRE(hits.size() == 2);        // both files embed; no duplicate of a.cc
  REQUIRE(hits[0].path == "a.cc");
  REQUIRE(hits[1].path == "b.cc");
}

TEST_CASE("A supplied summary skips the summarizer",
          "[code_indexer][cache]") {
  auto emb = std::make_shared<BagOfWordsEmbedder>();
  int calls = 0;
  SummarizeFn counting = [&calls](const SourceFile &) {
    ++calls;
    return std::string("generated");
  };
  CodeIndexer idx(CodeIndexerConfig{}, emb, counting);

  std::string cached = "persisted summary";
  std::string out;
  IndexStats hit = idx.IndexFile({"a.cc", "int x;\n"}, &cached, &out);
  REQUIRE(calls == 0);
  REQUIRE(hit.cache_hits == 1);
  REQUIRE(hit.summaries == 0);
  REQUIRE(out == "persisted summary");
  REQUIRE(idx.SummaryOf("a.cc") == "persisted summary");

  IndexStats miss = idx.IndexFile({"b.cc", "int y;\n"}, nullptr, &out);
  REQUIRE(calls == 1);
  REQUIRE(miss.summaries == 1);
  REQUIRE(out == "generated");
}

TEST_CASE("Fingerprint tracks content and summarizer settings",
          "[code_indexer][cache]") {
  CodeIndexerConfig a;
  a.summary_fingerprint = "model-a|prompt";
  CodeIndexerConfig b = a;
  b.summary_fingerprint = "model-b|prompt";
  CodeIndexer ia(a, nullptr, nullptr), ib(b, nullptr, nullptr);

  SourceFile f1{"x.cc", "int a;\n"};
  SourceFile f2{"x.cc", "int b;\n"};
  REQUIRE(ia.Fingerprint(f1) == ia.Fingerprint(f1));   // stable
  REQUIRE(ia.Fingerprint(f1) != ia.Fingerprint(f2));   // content changed
  REQUIRE(ia.Fingerprint(f1) != ib.Fingerprint(f1));   // model changed
  // The path is not part of the fingerprint: a moved file keeps its summary.
  REQUIRE(ia.Fingerprint(f1) == ia.Fingerprint({"y.cc", "int a;\n"}));
}

// ------------------------------------------------------------ embed cache

TEST_CASE("CachingEmbedder serves repeats from memory and from disk",
          "[code_indexer][cache]") {
  TempDir d;
  std::string file = (d.path / "emb.cache").string();
  auto inner = std::make_shared<BagOfWordsEmbedder>();
  {
    CachingEmbedder c(inner, "model-a", file);
    std::vector<std::vector<float>> v1, v2;
    REQUIRE(c.Embed({"alpha", "beta"}, &v1));
    REQUIRE(inner->seen.size() == 2);
    REQUIRE(c.Embed({"beta", "gamma"}, &v2));   // beta cached, gamma new
    REQUIRE(inner->seen.size() == 3);
    REQUIRE(v2[0] == v1[1]);
    REQUIRE(c.hits() == 1);
    REQUIRE(c.misses() == 3);
    REQUIRE(c.Save());
  }
  // A new process: everything is a hit, the inner embedder is not touched.
  auto inner2 = std::make_shared<BagOfWordsEmbedder>();
  CachingEmbedder c2(inner2, "model-a", file);
  std::vector<std::vector<float>> v;
  REQUIRE(c2.size() == 3);
  REQUIRE(c2.Embed({"gamma", "alpha"}, &v));
  REQUIRE(inner2->seen.empty());
  REQUIRE(v.size() == 2);
  REQUIRE(v[0].size() == 256);
}

TEST_CASE("CachingEmbedder keys on the model and fails closed",
          "[code_indexer][cache]") {
  TempDir d;
  std::string file = (d.path / "emb.cache").string();
  auto inner = std::make_shared<BagOfWordsEmbedder>();
  {
    CachingEmbedder c(inner, "model-a", file);
    std::vector<std::vector<float>> v;
    REQUIRE(c.Embed({"alpha"}, &v));
    REQUIRE(c.Save());
  }
  // Same text, different model: must NOT reuse model-a's vector.
  auto inner2 = std::make_shared<BagOfWordsEmbedder>();
  CachingEmbedder other(inner2, "model-b", file);
  std::vector<std::vector<float>> v;
  REQUIRE(other.Embed({"alpha"}, &v));
  REQUIRE(inner2->seen.size() == 1);
  // An inner failure returns false and leaves nothing half-cached.
  inner2->fail = true;
  REQUIRE_FALSE(other.Embed({"fresh text"}, &v));
  REQUIRE(v.empty());
  inner2->fail = false;
  REQUIRE(other.Embed({"fresh text"}, &v));
  REQUIRE(inner2->seen.size() == 2);
}

TEST_CASE("CachingEmbedder ignores a corrupt cache file",
          "[code_indexer][cache]") {
  TempDir d;
  d.Write("emb.cache", "this is not a cache file");
  auto inner = std::make_shared<BagOfWordsEmbedder>();
  CachingEmbedder c(inner, "m", (d.path / "emb.cache").string());
  REQUIRE(c.size() == 0);
  std::vector<std::vector<float>> v;
  REQUIRE(c.Embed({"x"}, &v));
  REQUIRE(c.Save());                // overwrites the junk with a valid file
  CachingEmbedder again(inner, "m", (d.path / "emb.cache").string());
  REQUIRE(again.size() == 1);
}

// ---------------------------------------------------------------- summarizer

TEST_CASE("MakeOllamaSummarizer sends path, prompt and content",
          "[code_indexer][summarize]") {
  OneShotHttpServer stub("HTTP/1.1 200 OK\r\n", "{\"response\":\"a summary\"}");
  OllamaSummarizeConfig c;
  c.endpoint = stub.Endpoint();
  c.model = "stub-model";
  SummarizeFn fn = MakeOllamaSummarizer(c);
  std::string s = fn({"dir/file.cc", "int MARKER_TOKEN;"});
  REQUIRE(s == "a summary");
  std::string body = stub.RequestBodies()[0];
  REQUIRE(body.find("dir/file.cc") != std::string::npos);
  REQUIRE(body.find("MARKER_TOKEN") != std::string::npos);
  REQUIRE(body.find("indexing a source file") != std::string::npos);
  stub.Stop();
}

TEST_CASE("MakeOllamaSummarizer returns empty when the model is unreachable",
          "[code_indexer][summarize]") {
  OllamaSummarizeConfig c;
  c.endpoint = "http://127.0.0.1:1";
  c.model = "stub-model";
  REQUIRE(MakeOllamaSummarizer(c)({"f.cc", "int x;"}).empty());
}

SIMPLE_TEST_MAIN()

TEST_CASE("CodeIndexer keyword+summary mode finds files by summary-only terms",
          "[code_indexer][pipeline]") {
  // "prefetcher" appears only in the summary, never in the path or source.
  SummarizeFn summ = [](const SourceFile &f) -> std::string {
    return f.path == "tiering/reorganize.cc" ? "A prefetcher for hot blobs."
                                              : "Unrelated helper.";
  };
  CodeIndexer idx(CodeIndexerConfig{}, nullptr, summ);
  idx.IndexFiles(SampleFiles());
  std::vector<SearchHit> hits;
  REQUIRE(idx.Query("prefetcher", 3, SearchMode::kKeyword, &hits));
  REQUIRE(hits.empty());  // plain keyword mode cannot see the summary
  REQUIRE(idx.Query("prefetcher", 3, SearchMode::kKeywordSummary, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].path == "tiering/reorganize.cc");
  // Source terms still match in the summary-augmented index.
  REQUIRE(idx.Query("HttpServer", 1, SearchMode::kKeywordSummary, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(hits[0].path == "net/http_server.cc");
}

TEST_CASE("ExtractReferences reads C++ includes and Python imports",
          "[code_indexer][graph]") {
  auto r = ExtractReferences(
      "#include <vector>\n  #include \"a/b.h\"\nimport os.path\n"
      "from pkg.mod import x, y\nint x;\n");
  REQUIRE(r.size() == 4);
  REQUIRE(r[0] == "vector");
  REQUIRE(r[1] == "a/b.h");
  REQUIRE(r[2] == "os/path");
  REQUIRE(r[3] == "pkg/mod");
}

TEST_CASE("FileGraph links includes and test/impl siblings",
          "[code_indexer][graph]") {
  std::vector<FileGraph::Node> n = {
      {0, "core/src/foo.cc", {"core/foo.h"}},
      {1, "core/include/core/foo.h", {}},
      {2, "test/test_foo.cc", {"core/foo.h"}},
      {3, "util/bar.cc", {"stdio.h"}},
  };
  FileGraph g;
  g.Build(n);
  REQUIRE(g.NumNodes() == 4);
  REQUIRE(g.Neighbors(3).empty());
  // 0-1 (include) and 0-2, 1-2 (include / stem foo).
  REQUIRE(g.Neighbors(0) == std::vector<uint32_t>({1, 2}));
  REQUIRE(g.Neighbors(1) == std::vector<uint32_t>({0, 2}));
  REQUIRE(g.NumEdges() == 3);
}

TEST_CASE("CodeIndexer keyword+graph surfaces a neighbour with no term match",
          "[code_indexer][graph]") {
  std::vector<SourceFile> files = {
      {"core/include/core/widget.h", "// zorbulate the quux\nclass Widget {};\n"},
      {"core/src/widget.cc",
       "#include \"core/widget.h\"\nvoid Widget::Run() { int a = 1; }\n"},
      {"net/other.cc", "int unrelated = 0;\n"},
  };
  CodeIndexer idx(CodeIndexerConfig{}, nullptr, nullptr);
  idx.IndexFiles(files);
  std::vector<SearchHit> hits;
  REQUIRE(idx.Query("zorbulate", 3, SearchMode::kKeyword, &hits));
  REQUIRE(hits.size() == 1);
  REQUIRE(idx.Query("zorbulate", 3, SearchMode::kKeywordGraph, &hits));
  REQUIRE(hits.size() == 2);
  REQUIRE(hits[0].path == "core/include/core/widget.h");
  REQUIRE(hits[1].path == "core/src/widget.cc");
  REQUIRE(idx.NumGraphEdges() == 1);
}

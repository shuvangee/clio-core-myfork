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


#ifndef CLIO_CAE_CODE_INDEXER_EMBED_CLIENT_H_
#define CLIO_CAE_CODE_INDEXER_EMBED_CLIENT_H_

/**
 * Text -> vector over HTTP, for the Code Indexer.
 *
 * The indexer talks to an `Embedder`, an interface with one method, so tests
 * can inject a deterministic fake and a later in-process model can drop in.
 * `HttpEmbedder` is the production implementation. It speaks the two wire
 * formats that cover the realistic local backends:
 *
 *   - kOllama : POST {endpoint}/api/embed       {"model","input":[...]}
 *               -> {"embeddings":[[...],...]}
 *   - kOpenAI : POST {endpoint}/v1/embeddings   {"model","input":[...]}
 *               -> {"data":[{"index":i,"embedding":[...]},...]}
 *               (llama.cpp server, vLLM, LM Studio, a sentence-transformers
 *               shim, ... all expose this one).
 *
 * Why both: the Ollama library lists only EmbeddingGemma v1 (300M, 2K context)
 * while EmbeddingGemma 2 ships as google/embeddinggemma-2 for
 * sentence-transformers, so the model a deployment can serve decides the wire
 * format. Request building and response parsing are exposed as plain
 * functions for the same testability reason as summarizer_rules.h.
 */

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace clio::cae::code_indexer {

/** Which HTTP embedding API the endpoint speaks. */
enum class EmbedApi { kOllama, kOpenAI };

/** Connection + request settings for an HttpEmbedder. */
struct EmbedConfig {
  /** Base URL, e.g. "http://127.0.0.1:11434". */
  std::string endpoint;
  /** Model name sent verbatim in the request. */
  std::string model = "embeddinggemma";
  EmbedApi api = EmbedApi::kOllama;
  /** Inputs per HTTP request. */
  size_t batch_size = 16;
  /** Whole-request timeout. A cold model load can take a while. */
  int timeout_seconds = 120;
};

/** Anything that can embed a batch of strings. */
class Embedder {
 public:
  virtual ~Embedder() = default;

  /**
   * Embed `texts`; on success `out` holds exactly texts.size() vectors in
   * order and the call returns true. On any failure it returns false and
   * `out` is cleared: a partially embedded batch is never returned, so the
   * caller cannot mis-pair vectors with inputs.
   */
  virtual bool Embed(const std::vector<std::string> &texts,
                     std::vector<std::vector<float>> *out) = 0;
};

/** JSON request body for one batch. */
std::string BuildEmbedRequest(EmbedApi api, const std::string &model,
                              const std::vector<std::string> &inputs);

/** Request path appended to the endpoint ("/api/embed" or "/v1/embeddings"). */
const char *EmbedPath(EmbedApi api);

/**
 * Parse a response body. Fails (false, `out` cleared) on malformed JSON, a
 * missing field, a count different from `expected`, an empty vector, or
 * vectors of unequal dimension.
 */
bool ParseEmbedResponse(EmbedApi api, const std::string &body, size_t expected,
                        std::vector<std::vector<float>> *out);

/** libcurl-backed Embedder. Blocking; one request per `batch_size` inputs. */
class HttpEmbedder : public Embedder {
 public:
  explicit HttpEmbedder(EmbedConfig cfg) : cfg_(std::move(cfg)) {}

  bool Embed(const std::vector<std::string> &texts,
             std::vector<std::vector<float>> *out) override;

 private:
  EmbedConfig cfg_;
};

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_EMBED_CLIENT_H_

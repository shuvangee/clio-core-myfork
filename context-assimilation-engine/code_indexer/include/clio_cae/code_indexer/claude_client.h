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


#ifndef CLIO_CAE_CODE_INDEXER_CLAUDE_CLIENT_H_
#define CLIO_CAE_CODE_INDEXER_CLAUDE_CLIENT_H_

/**
 * Anthropic Messages API client for the Code Indexer's model roles.
 *
 * The API key is read from an environment variable (default ANTHROPIC_API_KEY)
 * when the client is constructed and is never logged, printed or stored in a
 * file. Every successful call adds the API-reported token counts to Usage(),
 * so a comparison run can report exactly what it consumed.
 *
 * Request building and response parsing are plain functions so tests can
 * check them without a network; the endpoint is configurable so a test can
 * point the client at a local stub.
 */

#include <clio_cae/code_indexer/llm_roles.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace clio::cae::code_indexer {

struct ClaudeConfig {
  std::string endpoint = "https://api.anthropic.com";
  /** e.g. a Haiku or larger Claude model id; sent verbatim. */
  std::string model;
  std::string api_key_env = "ANTHROPIC_API_KEY";
  int max_tokens = 256;
  int timeout_seconds = 120;
  /** Extra attempts after a 429/5xx/overloaded answer or a transport error. */
  int retries = 3;
};

/** Cumulative counters across all calls of one client. */
struct ClaudeUsage {
  uint64_t calls = 0;
  uint64_t failures = 0;
  uint64_t input_tokens = 0;
  uint64_t output_tokens = 0;
};

/** JSON body for one single-turn request. */
std::string BuildClaudeRequest(const std::string &model, int max_tokens,
                               const std::string &prompt,
                               const std::string &payload);

/**
 * Pull the text and token counts out of a response body. False on malformed
 * JSON, an API error object, or no text block.
 */
bool ParseClaudeResponse(const std::string &body, std::string *text,
                         uint64_t *input_tokens, uint64_t *output_tokens);

class ClaudeClient {
 public:
  explicit ClaudeClient(ClaudeConfig cfg);

  /** False when the key variable was empty/unset: every Complete() fails. */
  bool HasKey() const { return !api_key_.empty(); }

  /** One request. Returns "" on any failure (counted in Usage().failures). */
  std::string Complete(const std::string &prompt, const std::string &payload);

  ClaudeUsage Usage() const;

 private:
  ClaudeConfig cfg_;
  std::string api_key_;
  std::atomic<uint64_t> calls_{0}, failures_{0}, in_{0}, out_{0};
};

/** An LlmFn over a shared client (the same client may back several roles). */
LlmFn MakeClaudeLlm(std::shared_ptr<ClaudeClient> client);

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CLAUDE_CLIENT_H_

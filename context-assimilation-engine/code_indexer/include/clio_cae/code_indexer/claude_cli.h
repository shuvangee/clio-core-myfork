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


#ifndef CLIO_CAE_CODE_INDEXER_CLAUDE_CLI_H_
#define CLIO_CAE_CODE_INDEXER_CLAUDE_CLI_H_

/**
 * Claude through the installed `claude` command (headless `claude -p`), i.e.
 * the user's existing Claude Code subscription login. This is the same route
 * Clio Coder's claude-code runtime takes; no API key and no credentials are
 * read or stored here. The prompt goes in on stdin, so no shell quoting and no
 * argv length limit apply.
 *
 * The child runs in a neutral working directory with tools, slash commands and
 * session persistence off, so no project instructions leak into the request.
 * POSIX only; on other platforms Complete() returns "" and counts a failure.
 */

#include <clio_cae/code_indexer/llm_roles.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace clio::cae::code_indexer {

struct ClaudeCliConfig {
  std::string binary = "claude";
  /** Alias or full model id passed to --model (e.g. "haiku", "sonnet"). */
  std::string model = "haiku";
  int timeout_seconds = 180;
  /** Working directory of the child ("" = /tmp). */
  std::string work_dir;
  /** Extra arguments appended after the defaults. */
  std::vector<std::string> extra_args;
};

struct ClaudeCliUsage {
  uint64_t calls = 0;
  uint64_t failures = 0;
  uint64_t input_tokens = 0;   // input + cache creation + cache read
  uint64_t output_tokens = 0;
  double cost_usd_reported = 0.0;  // notional for a subscription
};

/** Arguments after the binary name (exposed so a test can check them). */
std::vector<std::string> BuildClaudeCliArgs(const ClaudeCliConfig &cfg);

/** Extract "result" and token counts from `--output-format json` stdout. */
bool ParseClaudeCliOutput(const std::string &out, std::string *text,
                          uint64_t *input_tokens, uint64_t *output_tokens,
                          double *cost_usd);

class ClaudeCliClient {
 public:
  explicit ClaudeCliClient(ClaudeCliConfig cfg) : cfg_(std::move(cfg)) {}
  std::string Complete(const std::string &prompt, const std::string &payload);
  ClaudeCliUsage Usage() const;

 private:
  ClaudeCliConfig cfg_;
  std::atomic<uint64_t> calls_{0}, failures_{0}, in_{0}, out_{0};
  std::atomic<uint64_t> cost_micro_usd_{0};
};

LlmFn MakeClaudeCliLlm(std::shared_ptr<ClaudeCliClient> client);

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CLAUDE_CLI_H_

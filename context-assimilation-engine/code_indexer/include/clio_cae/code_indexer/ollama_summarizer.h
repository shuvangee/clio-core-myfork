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


#ifndef CLIO_CAE_CODE_INDEXER_OLLAMA_SUMMARIZER_H_
#define CLIO_CAE_CODE_INDEXER_OLLAMA_SUMMARIZER_H_

/**
 * The Code Indexer's summarizer, built on the CAE Summarizer's inference path.
 *
 * Reuses clio::cae::summarizer::GenerateSummary (and through it OllamaGenerate
 * and SplitPayload), so a file larger than the model's context is chunked and
 * the per-chunk summaries concatenated exactly as the PutBlob labeler does.
 * Kept out of code_indexer.h so the core pipeline does not link the CAE
 * runtime; this is the only piece that does.
 */

#include <clio_cae/code_indexer/code_indexer.h>
#include <clio_cae/code_indexer/llm_roles.h>
#include <clio_cae/code_indexer/query_rerank.h>

#include <string>

namespace clio::cae::code_indexer {

/** Which model summarizes, and how. */
struct OllamaSummarizeConfig {
  /** Base URL of an Ollama-compatible server (POST /api/generate). */
  std::string endpoint;
  std::string model;
  /** Prompt template; "" selects DefaultSummaryPrompt(). */
  std::string prompt;
  /** Per-request token window; also drives chunking of large files. */
  int context_length = 8192;
  /** Cap on each response, in tokens. 0 = uncapped. */
  int num_predict = 256;
};

/**
 * A SummarizeFn that sends each file to the configured model. The file path
 * is placed in front of the content so the model can name what it sees.
 * Returns "" on failure (the indexer still indexes the code).
 */
SummarizeFn MakeOllamaSummarizer(const OllamaSummarizeConfig &cfg);

/**
 * A JudgeFn backed by an Ollama-compatible model. The prompt contains the
 * query, the evolver's history text and the file (truncated to
 * `max_file_chars`); the model must answer with one integer 0..10. cfg.prompt
 * is ignored; cfg.num_predict should be small (the answer is one number).
 */
/**
 * Generic prompt+payload -> text over Ollama (cfg.num_predict caps the
 * answer). Lets the model roles in llm_roles.h run on a local model.
 */
LlmFn MakeOllamaLlm(const OllamaSummarizeConfig &cfg);

JudgeFn MakeOllamaJudge(const OllamaSummarizeConfig &cfg,
                        size_t max_file_chars = 6000);

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_OLLAMA_SUMMARIZER_H_

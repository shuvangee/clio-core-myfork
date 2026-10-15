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


#include <clio_cae/code_indexer/ollama_summarizer.h>

#include <clio_cae/summarizer/summarizer_inference.h>

#include <string>
#include <utility>

namespace clio::cae::code_indexer {

SummarizeFn MakeOllamaSummarizer(const OllamaSummarizeConfig &cfg) {
  namespace sm = clio::cae::summarizer;
  sm::LabelMatch rule;
  rule.tag_re_ = ".*";
  rule.blob_re_ = ".*";
  rule.model_ = cfg.model;
  rule.prompt_ = "code_indexer";
  rule.context_length_ = cfg.context_length;
  rule.num_predict_ = cfg.num_predict;
  std::string prompt = cfg.prompt.empty() ? DefaultSummaryPrompt() : cfg.prompt;
  std::string endpoint = cfg.endpoint;

  return [rule, prompt, endpoint](const SourceFile &f) {
    std::string payload = "File: " + f.path + "\n\n" + f.content;
    return sm::GenerateSummary(endpoint, rule, prompt, payload,
                               "code_indexer", f.path);
  };
}

JudgeFn MakeOllamaJudge(const OllamaSummarizeConfig &cfg,
                        size_t max_file_chars) {
  namespace sm = clio::cae::summarizer;
  sm::LabelMatch rule;
  rule.tag_re_ = ".*";
  rule.blob_re_ = ".*";
  rule.model_ = cfg.model;
  rule.prompt_ = "code_indexer_judge";
  rule.context_length_ = cfg.context_length;
  rule.num_predict_ = cfg.num_predict > 0 ? cfg.num_predict : 8;
  std::string endpoint = cfg.endpoint;

  return [rule, endpoint, max_file_chars](const std::string &query,
                                          const std::string &history,
                                          const SourceFile &f) -> double {
    std::string prompt =
        "You help a developer find source files. The developer's question "
        "is:\n\"" + query + "\"\n\n" + history +
        "Below is one source file. How likely is it that this is a file the "
        "developer needs for that question? Reply with ONLY one integer from "
        "0 (irrelevant) to 10 (exactly the file needed).";
    std::string payload = "File: " + f.path + "\n\n" +
                          f.content.substr(0, max_file_chars);
    std::string out = sm::GenerateSummary(endpoint, rule, prompt, payload,
                                          "code_indexer_judge", f.path);
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i] >= '0' && out[i] <= '9') {
        int v = out[i] - '0';
        if (v == 1 && i + 1 < out.size() && out[i + 1] == '0') v = 10;
        return static_cast<double>(v);
      }
    }
    return -1.0;
  };
}

}  // namespace clio::cae::code_indexer

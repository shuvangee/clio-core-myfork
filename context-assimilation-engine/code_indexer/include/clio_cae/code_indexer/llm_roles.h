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


#ifndef CLIO_CAE_CODE_INDEXER_LLM_ROLES_H_
#define CLIO_CAE_CODE_INDEXER_LLM_ROLES_H_

/**
 * The four jobs the Code Indexer gives a language model, expressed over one
 * tiny interface so any backend (Ollama, Claude, a test fake) can fill them:
 *
 *   summarize   file                      -> short summary
 *   judge       question + file           -> 0..10 relevance
 *   verify      file A + file B           -> are they really related?
 *   focus       question + file           -> what this file does FOR the question
 *
 * An LlmFn is "prompt + payload in, text out"; "" means the call failed.
 * Leaf header: no HTTP, no runtime.
 */

#include <clio_cae/code_indexer/code_indexer.h>
#include <clio_cae/code_indexer/query_rerank.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <string>

namespace clio::cae::code_indexer {

using LlmFn = std::function<std::string(const std::string &prompt,
                                        const std::string &payload)>;

inline std::string Head(const std::string &s, size_t n) {
  return s.size() > n ? s.substr(0, n) : s;
}

inline SummarizeFn MakeSummarizerFromLlm(LlmFn llm, std::string prompt = "") {
  if (prompt.empty()) prompt = DefaultSummaryPrompt();
  return [llm, prompt](const SourceFile &f) {
    return llm(prompt, "File: " + f.path + "\n\n" + f.content);
  };
}

/** Same prompt text as MakeOllamaJudge, so backends are comparable. */
inline JudgeFn MakeJudgeFromLlm(LlmFn llm, size_t max_chars = 6000) {
  return [llm, max_chars](const std::string &query, const std::string &history,
                          const SourceFile &f) -> double {
    std::string prompt =
        "You help a developer find source files. The developer's question "
        "is:\n\"" + query + "\"\n\n" + history +
        "Below is one source file. How likely is it that this is a file the "
        "developer needs for that question? Reply with ONLY one integer from "
        "0 (irrelevant) to 10 (exactly the file needed).";
    std::string out = llm(prompt, "File: " + f.path + "\n\n" + Head(f.content, max_chars));
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

/** yes -> true, no -> false; anything else (or a failed call) keeps the edge. */
inline EdgeVerifyFn MakeEdgeVerifierFromLlm(LlmFn llm, size_t max_chars = 3000) {
  return [llm, max_chars](const SourceFile &a, const SourceFile &b) -> bool {
    std::string prompt =
        "Below are two source files, A and B. Does one of them directly use, "
        "implement, declare, include or test the other, so that a developer "
        "changing one would likely need to look at the other? Reply with ONLY "
        "the word yes or the word no.";
    std::string payload = "File A: " + a.path + "\n" + Head(a.content, max_chars) +
                          "\n\n=====\n\nFile B: " + b.path + "\n" +
                          Head(b.content, max_chars);
    std::string out = llm(prompt, payload);
    size_t i = 0;
    while (i < out.size() && !std::isalpha(static_cast<unsigned char>(out[i]))) ++i;
    std::string w;
    for (; i < out.size() && std::isalpha(static_cast<unsigned char>(out[i])); ++i) {
      w.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(out[i]))));
    }
    if (w == "no") return false;
    return true;  // "yes", garbage and failures all keep the edge
  };
}

inline FocusFn MakeFocusFromLlm(LlmFn llm, size_t max_chars = 6000) {
  return [llm, max_chars](const std::string &query, const std::string &history,
                          const SourceFile &f) -> std::string {
    std::string prompt =
        "A developer is looking for:\n\"" + query + "\"\n\n" + history +
        "In at most two sentences, say what the source file below does that "
        "is relevant to that request. If it is not relevant, say 'Not "
        "relevant' and then name in a few words what it actually does.";
    return llm(prompt, "File: " + f.path + "\n\n" + Head(f.content, max_chars));
  };
}

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_LLM_ROLES_H_

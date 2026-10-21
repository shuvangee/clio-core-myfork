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


#include <clio_cae/code_indexer/claude_client.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

namespace clio::cae::code_indexer {

namespace {

size_t WriteCb(char *p, size_t sz, size_t n, void *ud) {
  static_cast<std::string *>(ud)->append(p, sz * n);
  return sz * n;
}

bool IsLoopback(const std::string &url) {
  return url.find("://127.0.0.1") != std::string::npos ||
         url.find("://localhost") != std::string::npos;
}

/** POST; returns the HTTP status (0 on transport error). Body goes to *resp. */
long Post(const std::string &url, const std::string &key,
          const std::string &body, int timeout_s, std::string *resp) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL *curl = curl_easy_init();
  if (curl == nullptr) return 0;
  struct curl_slist *h = nullptr;
  h = curl_slist_append(h, "Content-Type: application/json");
  h = curl_slist_append(h, "anthropic-version: 2023-06-01");
  std::string auth = "x-api-key: " + key;
  h = curl_slist_append(h, auth.c_str());
  resp->clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (IsLoopback(url)) curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
  CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(h);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK ? code : 0;
}

}  // namespace

std::string BuildClaudeRequest(const std::string &model, int max_tokens,
                               const std::string &prompt,
                               const std::string &payload) {
  nlohmann::json j;
  j["model"] = model;
  j["max_tokens"] = max_tokens;
  j["messages"] = nlohmann::json::array(
      {{{"role", "user"}, {"content", prompt + "\n\n" + payload}}});
  return j.dump();
}

bool ParseClaudeResponse(const std::string &body, std::string *text,
                         uint64_t *input_tokens, uint64_t *output_tokens) {
  try {
    nlohmann::json j = nlohmann::json::parse(body);
    if (j.contains("error") || !j.contains("content") ||
        !j["content"].is_array()) {
      return false;
    }
    std::string t;
    for (const auto &blk : j["content"]) {
      if (blk.value("type", "") == "text") t += blk.value("text", "");
    }
    if (t.empty()) return false;
    *text = std::move(t);
    *input_tokens = j.contains("usage") ? j["usage"].value("input_tokens", 0ULL) : 0;
    *output_tokens = j.contains("usage") ? j["usage"].value("output_tokens", 0ULL) : 0;
    return true;
  } catch (...) {
    return false;
  }
}

ClaudeClient::ClaudeClient(ClaudeConfig cfg) : cfg_(std::move(cfg)) {
  const char *k = std::getenv(cfg_.api_key_env.c_str());
  if (k != nullptr) api_key_ = k;
}

std::string ClaudeClient::Complete(const std::string &prompt,
                                   const std::string &payload) {
  ++calls_;
  if (api_key_.empty()) {
    ++failures_;
    return "";
  }
  const std::string url = cfg_.endpoint + "/v1/messages";
  const std::string body =
      BuildClaudeRequest(cfg_.model, cfg_.max_tokens, prompt, payload);
  for (int attempt = 0; attempt <= cfg_.retries; ++attempt) {
    std::string resp;
    long code = Post(url, api_key_, body, cfg_.timeout_seconds, &resp);
    if (code == 200) {
      std::string text;
      uint64_t in = 0, out = 0;
      if (ParseClaudeResponse(resp, &text, &in, &out)) {
        in_ += in;
        out_ += out;
        return text;
      }
      break;  // 200 but unusable: retrying will not help
    }
    bool transient = code == 0 || code == 429 || code >= 500;
    if (!transient) break;  // 4xx (bad key, bad model): do not hammer
    std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
  }
  ++failures_;
  return "";
}

ClaudeUsage ClaudeClient::Usage() const {
  return {calls_.load(), failures_.load(), in_.load(), out_.load()};
}

LlmFn MakeClaudeLlm(std::shared_ptr<ClaudeClient> client) {
  return [client](const std::string &prompt, const std::string &payload) {
    return client->Complete(prompt, payload);
  };
}

}  // namespace clio::cae::code_indexer

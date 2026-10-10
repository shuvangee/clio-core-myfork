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


#include <clio_cae/code_indexer/embed_client.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace clio::cae::code_indexer {

namespace {

void EnsureCurlInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteToStringCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// Loopback must never go through a site http_proxy (HPC nodes export one);
// whatever no_proxy already lists is preserved because CURLOPT_NOPROXY
// replaces libcurl's own reading of it. Same rule as the summarizer's
// label_client, kept local so this library does not link the runtime.
std::string NoProxyList() {
  const char *env = std::getenv("no_proxy");
  if (env == nullptr || *env == '\0') env = std::getenv("NO_PROXY");
  std::string list = "localhost,127.0.0.1,::1";
  if (env != nullptr && *env != '\0') {
    list += ",";
    list += env;
  }
  return list;
}

/** POST `body` as JSON. Returns true only for HTTP 200. */
bool PostJson(const std::string &url, const std::string &body, int timeout_s,
              std::string *response) {
  EnsureCurlInit();
  CURL *curl = curl_easy_init();
  if (curl == nullptr) return false;

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string no_proxy = NoProxyList();
  response->clear();

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToStringCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROXY, no_proxy.c_str());

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK && http_code == 200;
}

/** Pull one embedding array out of a JSON value into floats. */
bool ToFloats(const nlohmann::json &arr, std::vector<float> *v) {
  if (!arr.is_array() || arr.empty()) return false;
  v->clear();
  v->reserve(arr.size());
  for (const auto &x : arr) {
    if (!x.is_number()) return false;
    v->push_back(x.get<float>());
  }
  return true;
}

}  // namespace

const char *EmbedPath(EmbedApi api) {
  return api == EmbedApi::kOllama ? "/api/embed" : "/v1/embeddings";
}

std::string BuildEmbedRequest(EmbedApi, const std::string &model,
                              const std::vector<std::string> &inputs) {
  // Both formats take the same two fields; they differ in path and reply.
  nlohmann::json body = {{"model", model}, {"input", inputs}};
  return body.dump();
}

bool ParseEmbedResponse(EmbedApi api, const std::string &body, size_t expected,
                        std::vector<std::vector<float>> *out) {
  out->clear();
  try {
    nlohmann::json j = nlohmann::json::parse(body);
    std::vector<std::vector<float>> vecs(expected);
    if (api == EmbedApi::kOllama) {
      if (!j.contains("embeddings") || !j["embeddings"].is_array() ||
          j["embeddings"].size() != expected) {
        return false;
      }
      for (size_t i = 0; i < expected; ++i) {
        if (!ToFloats(j["embeddings"][i], &vecs[i])) return false;
      }
    } else {
      if (!j.contains("data") || !j["data"].is_array() ||
          j["data"].size() != expected) {
        return false;
      }
      // Servers may return items out of order; `index` is authoritative.
      std::vector<bool> seen(expected, false);
      for (const auto &item : j["data"]) {
        size_t idx = item.value("index", static_cast<size_t>(expected));
        if (idx >= expected || seen[idx] || !item.contains("embedding")) {
          return false;
        }
        if (!ToFloats(item["embedding"], &vecs[idx])) return false;
        seen[idx] = true;
      }
    }
    for (const auto &v : vecs) {
      if (v.size() != vecs[0].size()) return false;
    }
    *out = std::move(vecs);
    return true;
  } catch (const std::exception &) {
    out->clear();
    return false;
  }
}

bool HttpEmbedder::Embed(const std::vector<std::string> &texts,
                         std::vector<std::vector<float>> *out) {
  out->clear();
  if (cfg_.endpoint.empty() || cfg_.model.empty()) return false;
  std::string url = cfg_.endpoint;
  if (url.back() == '/') url.pop_back();
  url += EmbedPath(cfg_.api);

  size_t step = cfg_.batch_size == 0 ? 1 : cfg_.batch_size;
  std::vector<std::vector<float>> all;
  all.reserve(texts.size());
  for (size_t off = 0; off < texts.size(); off += step) {
    size_t end = std::min(texts.size(), off + step);
    std::vector<std::string> batch(texts.begin() + off, texts.begin() + end);
    std::string reply;
    std::vector<std::vector<float>> part;
    if (!PostJson(url, BuildEmbedRequest(cfg_.api, cfg_.model, batch),
                  cfg_.timeout_seconds, &reply) ||
        !ParseEmbedResponse(cfg_.api, reply, batch.size(), &part)) {
      out->clear();
      return false;
    }
    for (auto &v : part) all.push_back(std::move(v));
  }
  *out = std::move(all);
  return true;
}

}  // namespace clio::cae::code_indexer

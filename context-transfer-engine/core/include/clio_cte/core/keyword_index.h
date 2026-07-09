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

#ifndef CLIO_CTE_CORE_KEYWORD_INDEX_H_
#define CLIO_CTE_CORE_KEYWORD_INDEX_H_

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace clio::cte::core {

/**
 * Thread-safe in-memory inverted index for CTE blob contents.
 *
 * The forward document map retains term frequencies so an update can remove
 * stale postings without scanning the whole index. The reverse map provides
 * keyword-to-document candidate lookup for BM25 ranking.
 */
class KeywordIndex {
 public:
  /** Indexed representation of one blob. */
  struct Document {
    std::uint32_t tag_major_ = 0;
    std::uint32_t tag_minor_ = 0;
    std::string blob_name_;
    std::unordered_map<std::string, std::size_t> term_frequencies_;
    std::size_t token_count_ = 0;
  };

  /** Consistent query snapshot used outside the index lock. */
  struct Snapshot {
    std::vector<Document> documents_;
    std::unordered_map<std::string, std::size_t> document_frequencies_;
    std::size_t corpus_document_count_ = 0;
    std::size_t corpus_token_count_ = 0;
  };

  /**
   * Split bytes into lowercase ASCII alphanumeric terms.
   *
   * @param data Raw blob or query bytes.
   * @param size Number of bytes available at data.
   * @return Terms with at least two characters.
   */
  static std::vector<std::string> Tokenize(const char *data, std::size_t size) {
    std::vector<std::string> tokens;
    std::string current;
    current.reserve(16);
    for (std::size_t i = 0; i < size; ++i) {
      const unsigned char value = static_cast<unsigned char>(data[i]);
      if (std::isalnum(value) != 0) {
        current.push_back(
            static_cast<char>(std::tolower(static_cast<int>(value))));
      } else {
        AppendToken(current, tokens);
      }
    }
    AppendToken(current, tokens);
    return tokens;
  }

  /**
   * Insert or replace the indexed contents of a blob.
   *
   * @param tag_major Major component of the blob's tag ID.
   * @param tag_minor Minor component of the blob's tag ID.
   * @param blob_name Name of the blob within its tag.
   * @param data Complete current blob bytes.
   * @param size Number of bytes available at data.
   */
  void Update(std::uint32_t tag_major, std::uint32_t tag_minor,
              const std::string &blob_name, const char *data,
              std::size_t size) {
    Document document;
    document.tag_major_ = tag_major;
    document.tag_minor_ = tag_minor;
    document.blob_name_ = blob_name;
    auto tokens = Tokenize(data, size);
    document.token_count_ = tokens.size();
    for (const auto &token : tokens) {
      ++document.term_frequencies_[token];
    }

    const std::string key = MakeKey(tag_major, tag_minor, blob_name);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    RemoveLocked(key);
    total_token_count_ += document.token_count_;
    auto inserted = documents_.emplace(key, std::move(document));
    for (const auto &term : inserted.first->second.term_frequencies_) {
      postings_[term.first].insert(key);
    }
  }

  /**
   * Remove one blob and all of its reverse postings.
   *
   * @param tag_major Major component of the blob's tag ID.
   * @param tag_minor Minor component of the blob's tag ID.
   * @param blob_name Name of the blob within its tag.
   * @return True when the blob was present in the index.
   */
  bool Remove(std::uint32_t tag_major, std::uint32_t tag_minor,
              const std::string &blob_name) {
    const std::string key = MakeKey(tag_major, tag_minor, blob_name);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return RemoveLocked(key);
  }

  /**
   * Snapshot documents containing at least one query term.
   *
   * @param query_terms Unique normalized query terms.
   * @return Candidate documents and global BM25 corpus statistics.
   */
  Snapshot Find(const std::unordered_set<std::string> &query_terms) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Snapshot snapshot;
    snapshot.corpus_document_count_ = documents_.size();
    snapshot.corpus_token_count_ = total_token_count_;

    std::unordered_set<std::string> candidate_keys;
    if (query_terms.empty()) {
      candidate_keys.reserve(documents_.size());
      for (const auto &document : documents_) {
        candidate_keys.insert(document.first);
      }
    }
    for (const auto &term : query_terms) {
      auto posting = postings_.find(term);
      if (posting == postings_.end()) {
        continue;
      }
      snapshot.document_frequencies_[term] = posting->second.size();
      candidate_keys.insert(posting->second.begin(), posting->second.end());
    }
    snapshot.documents_.reserve(candidate_keys.size());
    for (const auto &key : candidate_keys) {
      auto document = documents_.find(key);
      if (document != documents_.end()) {
        snapshot.documents_.push_back(document->second);
      }
    }
    return snapshot;
  }

  /** Remove every document and posting from the index. */
  void Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    postings_.clear();
    documents_.clear();
    total_token_count_ = 0;
  }

  /**
   * Return the number of indexed blobs.
   *
   * @return Current document count.
   */
  std::size_t Size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return documents_.size();
  }

 private:
  /**
   * Append a completed token if it meets the minimum length.
   *
   * @param token Token accumulator to consume.
   * @param tokens Destination token vector.
   */
  static void AppendToken(std::string &token,
                          std::vector<std::string> &tokens) {
    if (token.size() >= 2) {
      tokens.push_back(std::move(token));
    }
    token.clear();
  }

  /**
   * Construct the stable internal identity for a blob.
   *
   * @param tag_major Major component of the tag ID.
   * @param tag_minor Minor component of the tag ID.
   * @param blob_name Blob name within the tag.
   * @return Collision-free length-delimited key.
   */
  static std::string MakeKey(std::uint32_t tag_major, std::uint32_t tag_minor,
                             const std::string &blob_name) {
    return std::to_string(tag_major) + "." + std::to_string(tag_minor) + "." +
           std::to_string(blob_name.size()) + ":" + blob_name;
  }

  /**
   * Remove a document while the caller holds the exclusive lock.
   *
   * @param key Internal document key.
   * @return True when the document existed.
   */
  bool RemoveLocked(const std::string &key) {
    auto document = documents_.find(key);
    if (document == documents_.end()) {
      return false;
    }
    for (const auto &term : document->second.term_frequencies_) {
      auto posting = postings_.find(term.first);
      if (posting == postings_.end()) {
        continue;
      }
      posting->second.erase(key);
      if (posting->second.empty()) {
        postings_.erase(posting);
      }
    }
    total_token_count_ -= document->second.token_count_;
    documents_.erase(document);
    return true;
  }

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::unordered_set<std::string>> postings_;
  std::unordered_map<std::string, Document> documents_;
  std::size_t total_token_count_ = 0;
};

}  // namespace clio::cte::core

#endif  // CLIO_CTE_CORE_KEYWORD_INDEX_H_

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


#ifndef CLIO_CAE_CODE_INDEXER_CODE_SCANNER_H_
#define CLIO_CAE_CODE_INDEXER_CODE_SCANNER_H_

/**
 * Turning a directory tree into the files and chunks the Code Indexer works on.
 *
 * Leaf header: standard library only. No runtime, no CTE, no HTTP, so the
 * walk/filter/chunk rules are plain functions a unit test can drive directly.
 */

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace clio::cae::code_indexer {

/** One source file read off disk. `path` is relative to the scan root. */
struct SourceFile {
  std::string path;
  std::string content;
};

/** A contiguous run of lines of one file (1-based, inclusive). */
struct CodeChunk {
  size_t start_line = 0;
  size_t end_line = 0;
  std::string text;
};

/** What a scan keeps and what it skips. */
struct ScanOptions {
  /** Extensions (with the dot) that count as source. */
  std::set<std::string> extensions = {".c",  ".cc",  ".cpp", ".cxx", ".h",
                                      ".hh", ".hpp", ".cu",  ".cuh", ".py",
                                      ".rs", ".go",  ".java", ".js", ".ts"};
  /** Directory names that are never descended into. */
  std::set<std::string> ignore_dirs = {
      ".git",         "build",    "node_modules", "external", "third_party",
      "__pycache__",  ".venv",    "venv",         "dist"};
  /** Files larger than this are skipped (generated code, data dumps). */
  size_t max_file_bytes = 256 * 1024;
  /** 0 = unlimited. The scan is sorted, so a cap is deterministic. */
  size_t max_files = 0;
};

namespace detail {

/** A NUL byte in the first 4 KiB marks a file as binary. */
inline bool LooksBinary(const std::string &content) {
  size_t n = std::min<size_t>(content.size(), 4096);
  return content.find('\0') < n;
}

inline bool ReadWholeFile(const std::filesystem::path &p, std::string *out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

}  // namespace detail

/**
 * Walk `root` and return every kept source file, sorted by relative path.
 *
 * Skips ignored directories, files with other extensions, files over
 * max_file_bytes, binary files, and unreadable files. A nonexistent root
 * yields an empty result.
 */
inline std::vector<SourceFile> ScanDirectory(const std::string &root,
                                             const ScanOptions &opts = {}) {
  namespace fs = std::filesystem;
  std::vector<SourceFile> out;
  std::error_code ec;
  fs::path base(root);
  if (!fs::is_directory(base, ec)) return out;

  std::vector<fs::path> found;
  fs::recursive_directory_iterator it(
      base, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::directory_entry &e = *it;
    if (e.is_directory(ec)) {
      if (opts.ignore_dirs.count(e.path().filename().string()) != 0) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!e.is_regular_file(ec)) continue;
    if (opts.extensions.count(e.path().extension().string()) == 0) continue;
    if (e.file_size(ec) > opts.max_file_bytes) continue;
    found.push_back(e.path());
  }
  std::sort(found.begin(), found.end());

  for (const fs::path &p : found) {
    if (opts.max_files != 0 && out.size() >= opts.max_files) break;
    SourceFile f;
    if (!detail::ReadWholeFile(p, &f.content)) continue;
    if (detail::LooksBinary(f.content)) continue;
    f.path = fs::relative(p, base, ec).generic_string();
    out.push_back(std::move(f));
  }
  return out;
}

/**
 * Split a file into line-window chunks for embedding.
 *
 * Windows are `chunk_lines` long; a window whose text exceeds `max_chars` is
 * cut at the last newline before the limit (or hard-cut when a single line is
 * longer than the limit), so no chunk can overflow the embedder's context.
 * At most `max_chunks` are returned; the rest of a very long file is not
 * embedded. An empty file yields no chunks.
 */
inline std::vector<CodeChunk> ChunkLines(const std::string &content,
                                         size_t chunk_lines, size_t max_chars,
                                         size_t max_chunks) {
  std::vector<CodeChunk> chunks;
  if (content.empty() || chunk_lines == 0 || max_chars == 0) return chunks;

  CodeChunk cur;
  size_t line_no = 0;
  size_t lines_in_cur = 0;
  auto flush = [&]() {
    if (!cur.text.empty()) chunks.push_back(std::move(cur));
    cur = CodeChunk();
    lines_in_cur = 0;
  };

  size_t pos = 0;
  while (pos < content.size() && chunks.size() < max_chunks) {
    size_t nl = content.find('\n', pos);
    size_t end = (nl == std::string::npos) ? content.size() : nl + 1;
    std::string line = content.substr(pos, end - pos);
    pos = end;
    ++line_no;

    // A single line longer than the budget is hard-cut into pieces.
    while (line.size() > max_chars) {
      flush();
      cur.start_line = cur.end_line = line_no;
      cur.text = line.substr(0, max_chars);
      line.erase(0, max_chars);
      flush();
      if (chunks.size() >= max_chunks) return chunks;
    }
    if (!cur.text.empty() && (lines_in_cur >= chunk_lines ||
                              cur.text.size() + line.size() > max_chars)) {
      flush();
    }
    if (cur.text.empty()) cur.start_line = line_no;
    cur.text += line;
    cur.end_line = line_no;
    ++lines_in_cur;
  }
  if (chunks.size() < max_chunks) flush();
  return chunks;
}

}  // namespace clio::cae::code_indexer

#endif  // CLIO_CAE_CODE_INDEXER_CODE_SCANNER_H_

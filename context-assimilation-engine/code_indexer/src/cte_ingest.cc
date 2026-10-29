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


#include <clio_cae/code_indexer/cte_ingest.h>

#include <clio_cte/filesystem/filesystem_client.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace clio::cae::code_indexer {

IngestStats IngestTree(const std::string &local_root,
                       const std::string &cte_root, const ScanOptions &opts) {
  IngestStats st;
  std::vector<SourceFile> files = ScanDirectory(local_root, opts);
  if (!clio::cte::filesystem::CLIO_CFS_CLIENT_INIT()) {
    st.failures = files.size();
    return st;
  }
  auto *fs = CLIO_CFS_CLIENT;
  std::string base = cte_root;
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  for (const SourceFile &f : files) {
    const std::string path = (base == "/" ? "" : base) + "/" + f.path;
    int fd = fs->OpenFd(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
      ++st.failures;
      continue;
    }
    size_t off = 0;
    bool ok = true;
    while (off < f.content.size()) {
      auto n = fs->PwriteFd(fd, f.content.data() + off, f.content.size() - off,
                            static_cast<clio::cte::filesystem::FsOff>(off));
      if (n <= 0) {
        ok = false;
        break;
      }
      off += static_cast<size_t>(n);
    }
    if (fs->CloseFd(fd) != 0) ok = false;
    if (ok) {
      ++st.files;
      st.bytes += f.content.size();
    } else {
      ++st.failures;
    }
  }
  return st;
}

std::vector<SourceFile> ReadTreeFromCte(const std::string &cte_root,
                                        const ScanOptions &opts) {
  std::vector<SourceFile> out;
  if (!clio::cte::filesystem::CLIO_CFS_CLIENT_INIT()) return out;
  auto *fs = CLIO_CFS_CLIENT;
  std::string root = cte_root;
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  if (root.empty()) root = "/";
  const std::string prefix = root == "/" ? "/" : root + "/";

  std::vector<std::string> stack = {root};
  std::vector<std::pair<std::string, std::string>> found;
  while (!stack.empty()) {
    std::string dir = std::move(stack.back());
    stack.pop_back();
    std::vector<std::string> names;
    if (fs->ReaddirPath(dir, &names) != 0) continue;
    std::sort(names.begin(), names.end());
    for (const std::string &entry : names) {
      const std::string path =
          (!entry.empty() && entry[0] == '/')
              ? entry
              : (dir == "/" ? "/" : dir + "/") + entry;
      const std::string name = path.substr(path.find_last_of('/') + 1);
      // StatPath reports every clio:: entry as a regular file, so ask the
      // chimod directly whether this one is a directory.
      auto ga = fs->AsyncGetattr(path);
      ga.Wait();
      if (ga->GetReturnCode() != 0 || ga->exists_ == 0 ||
          ga->is_symlink_ != 0) {
        continue;
      }
      if (ga->is_dir_ != 0) {
        if (opts.ignore_dirs.count(name) == 0) stack.push_back(path);
        continue;
      }
      size_t dot = name.find_last_of('.');
      if (dot == std::string::npos ||
          opts.extensions.count(name.substr(dot)) == 0) {
        continue;
      }
      const size_t n = static_cast<size_t>(ga->size_);
      if (n == 0 || n > opts.max_file_bytes) continue;
      int fd = fs->OpenFd(path, O_RDONLY, 0);
      if (fd < 0) continue;
      std::string content(n, '\0');
      size_t got = 0;
      while (got < n) {
        auto r = fs->PreadFd(fd, &content[got], n - got,
                             static_cast<clio::cte::filesystem::FsOff>(got));
        if (r <= 0) break;
        got += static_cast<size_t>(r);
      }
      fs->CloseFd(fd);
      content.resize(got);
      if (content.empty() ||
          content.find('\0') < std::min<size_t>(content.size(), 4096)) {
        continue;
      }
      found.emplace_back(path, std::move(content));
    }
  }
  std::sort(found.begin(), found.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  for (auto &f : found) {
    if (opts.max_files != 0 && out.size() >= opts.max_files) break;
    SourceFile sf;
    sf.path = f.first.compare(0, prefix.size(), prefix) == 0
                  ? f.first.substr(prefix.size())
                  : f.first;
    sf.content = std::move(f.second);
    out.push_back(std::move(sf));
  }
  return out;
}

}  // namespace clio::cae::code_indexer

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

#ifndef CLIO_CTE_ADAPTER_METADATA_MANAGER_H
#define CLIO_CTE_ADAPTER_METADATA_MANAGER_H

#include <cstdio>
#include <unordered_map>

#include "filesystem_io_client.h"
#include "clio_ctp/thread/lock.h"

namespace clio::cae {

/**
 * Hardcoded adapter page size. Defined here (instead of using
 * kAdapterPageSize from filesystem.h) so this header stays free of the
 * filesystem.h <-> filesystem_mdm.h include cycle.
 */
static constexpr size_t kMdmAdapterPageSize = 1024 * 1024;


// MDM operation constants for lock priority
const int kMDM_Create = 1;
const int kMDM_Update = 2;
const int kMDM_Delete = 3;
const int kMDM_Find = 4;
const int kMDM_Find2 = 5;

/**
 * Metadata manager for POSIX adapter
 */
class MetadataManager {
private:
  std::unordered_map<std::string, std::list<File>>
      path_to_hermes_file_; /**< Map to determine if path is buffered. */
  std::unordered_map<File, std::shared_ptr<AdapterStat>>
      hermes_file_to_stat_; /**< Map for metadata */
  ctp::RwLock lock_;             /**< Lock to synchronize MD updates*/

public:
  std::unordered_map<uint64_t, FsAsyncTask *>
      request_map_;           /**< Map for async FS requests */
  FsIoClientMetadata fs_mdm_; /**< Context needed for I/O clients */

  /** Constructor */
  MetadataManager() = default;

  /** Get the current adapter mode */
  AdapterMode GetBaseAdapterMode() {
    ctp::ScopedRwReadLock md_lock(lock_, 1);
    return AdapterMode::kDefault;
  }

  /** Get the adapter mode for a particular file */
  AdapterMode GetAdapterMode(const std::string &path) {
    (void)path;
    ctp::ScopedRwReadLock md_lock(lock_, 2);
    return AdapterMode::kDefault;
  }

  /** Get the adapter page size for a particular file (uniform 1 MiB). */
  size_t GetAdapterPageSize(const std::string &path) {
    (void)path;
    return kMdmAdapterPageSize;
  }

  /**
   * Create a metadata entry for filesystem adapters given File handler.
   * @param f original file handler of the file on the destination
   * filesystem.
   * @param stat POSIX Adapter version of Stat data structure.
   * @return    true, if operation was successful.
   *            false, if operation was unsuccessful.
   */
  bool Create(const File &f, std::shared_ptr<AdapterStat> &stat) {
    HLOG(kDebug, "Create metadata for file handler");
    ctp::ScopedRwWriteLock md_lock(lock_, kMDM_Create);
    if (path_to_hermes_file_.find(stat->path_) == path_to_hermes_file_.end()) {
      path_to_hermes_file_.emplace(stat->path_, std::list<File>());
    }
    path_to_hermes_file_[stat->path_].emplace_back(f);
    auto ret = hermes_file_to_stat_.emplace(f, std::move(stat));
    return ret.second;
  }

  /**
   * Update existing metadata entry for filesystem adapters.
   * @param f original file handler of the file on the destination.
   * @param stat POSIX Adapter version of Stat data structure.
   * @return    true, if operation was successful.
   *            false, if operation was unsuccessful or entry doesn't exist.
   */
  bool Update(const File &f, const AdapterStat &stat) {
    HLOG(kDebug, "Update metadata for file handler");
    ctp::ScopedRwWriteLock md_lock(lock_, kMDM_Update);
    auto iter = hermes_file_to_stat_.find(f);
    if (iter != hermes_file_to_stat_.end()) {
      *(*iter).second = stat;
      return true;
    } else {
      return false;
    }
  }

  /**
   * Delete existing metadata entry for for filesystem adapters.
   * @param f original file handler of the file on the destination.
   * @return    true, if operation was successful.
   *            false, if operation was unsuccessful.
   */
  bool Delete(const std::string &path, const File &f) {
    HLOG(kDebug, "Delete metadata for file handler");
    ctp::ScopedRwWriteLock md_lock(lock_, kMDM_Delete);
    auto iter = hermes_file_to_stat_.find(f);
    if (iter != hermes_file_to_stat_.end()) {
      hermes_file_to_stat_.erase(iter);
      auto &list = path_to_hermes_file_[path];
      auto f_iter = std::find(list.begin(), list.end(), f);
      path_to_hermes_file_[path].erase(f_iter);
      if (list.size() == 0) {
        path_to_hermes_file_.erase(path);
      }
      return true;
    } else {
      return false;
    }
  }

  /**
   * Find the clio file relating to a path.
   * @param path the path being checked
   * @return The clio file.
   * */
  std::list<File> *Find(const std::string &path) {
    std::string canon_path = stdfs::absolute(path).string();
    ctp::ScopedRwReadLock md_lock(lock_, kMDM_Find);
    auto iter = path_to_hermes_file_.find(canon_path);
    if (iter == path_to_hermes_file_.end())
      return nullptr;
    else
      return &iter->second;
  }

  /**
   * Find existing metadata entry for filesystem adapters.
   * @param f original file handler of the file on the destination.
   * @return    The metadata entry if exist.
   *            The bool in pair indicated whether metadata entry exists.
   */
  std::shared_ptr<AdapterStat> Find(const File &f) {
    ctp::ScopedRwReadLock md_lock(lock_, kMDM_Find2);
    auto iter = hermes_file_to_stat_.find(f);
    if (iter == hermes_file_to_stat_.end())
      return nullptr;
    else
      return iter->second;
  }

  /**
   * Add a request to the request map.
   * */
  void EmplaceTask(uint64_t id, FsAsyncTask *task) {
    ctp::ScopedRwWriteLock md_lock(lock_, 0);
    request_map_.emplace(id, task);
  }

  /**
   * Find a request in the request map.
   * */
  FsAsyncTask *FindTask(uint64_t id) {
    ctp::ScopedRwReadLock md_lock(lock_, 0);
    auto iter = request_map_.find(id);
    if (iter == request_map_.end()) {
      return nullptr;
    } else {
      return iter->second;
    }
  }

  /**
   * Delete a request in the request map.
   * */
  void DeleteTask(uint64_t id) {
    ctp::ScopedRwWriteLock md_lock(lock_, 0);
    auto iter = request_map_.find(id);
    if (iter != request_map_.end()) {
      request_map_.erase(iter);
    }
  }
};

} // namespace clio::cae

// Global pointer-based singleton
#include "clio_ctp/util/singleton.h"

namespace clio::cae {
CTP_DEFINE_GLOBAL_PTR_VAR_H(MetadataManager, g_fs_metadata_manager);
}

#define CLIO_CTE_FS_METADATA_MANAGER (CTP_GET_GLOBAL_PTR_VAR(clio::cae::MetadataManager, clio::cae::g_fs_metadata_manager))
#define CLIO_CTE_FS_METADATA_MANAGER_T clio::cae::MetadataManager *

#endif // CLIO_CTE_ADAPTER_METADATA_MANAGER_H

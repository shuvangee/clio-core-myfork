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

#ifndef CLIO_CTE_ADAPTER_FILESYSTEM_FILESYSTEM_H_
#define CLIO_CTE_ADAPTER_FILESYSTEM_FILESYSTEM_H_

#ifndef O_TMPFILE
#define O_TMPFILE 0x0
#endif

#include <ftw.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
// #include <mpi.h>

#include <filesystem>
#include <future>
#include <set>
#include <string>

#include "adapter/adapter_types.h"
#include "adapter/mapper/mapper_factory.h"
#include "clio_runtime/clio_runtime.h"
#include "filesystem_io_client.h"
#include "filesystem_mdm.h"
#include "clio_cte/core/content_transfer_engine.h"
#include "clio_cte/core/core_client.h"
#include "clio_cte/core/core_tasks.h"

namespace clio::cae {

/** The maximum length of a posix path */
static inline const int kMaxPathLen = 4096;

/**
 * Logical-namespace marker that opts a path into CTE interception.
 *
 * Any path component that starts with "clio::" turns the entire open()
 * (or stat/MPI_File_open/fopen/...) call into a CTE-routed I/O. Three
 * accepted shapes:
 *
 *   clio::/tmp/foo.dat              ← marker is the whole leading segment
 *   data/clio::foo.dat              ← marker on the trailing component
 *   /abs/cwd/bash/clio::data/foo    ← marker on an interior component
 *                                    (this is what Python's
 *                                     rundir.joinpath("clio::data/foo")
 *                                     produces — common with wfbench
 *                                     and friends that resolve relative
 *                                     paths before calling open())
 *
 * The marker is stripped from the path before it reaches the backend
 * filesystem and the CTE tag manager, so on-disk filenames stay clean
 * (e.g. "/abs/bash/clio::data/foo" → "/abs/bash/data/foo").
 */
static constexpr const char kClioPrefix[] = "clio::";
static constexpr size_t kClioPrefixLen = sizeof(kClioPrefix) - 1;  // 6

/** Return the byte offset where "clio::" appears as a path-component
 *  prefix in @p path, or std::string::npos if it doesn't.
 *  A component-prefix appearance is either:
 *    - position 0 (the whole path begins with "clio::"), or
 *    - immediately after a '/' (some path component begins with "clio::").
 *  Matches the first occurrence only — paths with more than one
 *  "clio::" marker are unusual and only the first is stripped.
 */
inline size_t FindClioMarker(const std::string &path) {
  if (path.size() >= kClioPrefixLen &&
      path.compare(0, kClioPrefixLen, kClioPrefix) == 0) {
    return 0;
  }
  size_t cur = 0;
  while ((cur = path.find('/', cur)) != std::string::npos) {
    if (cur + 1 + kClioPrefixLen <= path.size() &&
        path.compare(cur + 1, kClioPrefixLen, kClioPrefix) == 0) {
      return cur + 1;
    }
    ++cur;
  }
  return std::string::npos;
}

inline bool HasClioPrefix(const std::string &path) {
  return FindClioMarker(path) != std::string::npos;
}

inline std::string StripClioPrefix(const std::string &path) {
  size_t pos = FindClioMarker(path);
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(0, pos) + path.substr(pos + kClioPrefixLen);
}

/** Hardcoded adapter page size (1 MiB). */
static constexpr size_t kAdapterPageSize = 1024 * 1024;

/**
 * Process-wide queue of pending AsyncDelTag (close-time) futures.
 *
 * Each Filesystem::Remove fires AsyncDelTag and pushes the resulting
 * future here instead of waiting on it. Filesystem::Open lazily polls
 * the first N entries with Future::Wait(0) (non-blocking) at the start
 * of each new open call, removing any that have completed. The queue
 * keeps close-time RPCs off the user's critical path; the next open
 * occurrence does a tiny amount of bookkeeping work.
 */
class PendingCloses {
 public:
  static PendingCloses &Get() {
    static PendingCloses inst;
    return inst;
  }

  void Push(chi::Future<clio::cte::core::DelTagTask> &&fut) {
    std::lock_guard<std::mutex> lock(mu_);
    closes_.emplace_back(std::move(fut));
  }

  /**
   * Poll the first `n` pending closes with Wait(0) and remove any that
   * have completed. n=0 means scan the whole queue. Default callers
   * pass 3 — enough to keep the queue bounded under typical workflow
   * patterns without burning many cycles per open.
   */
  void ReapN(size_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    size_t limit = (n == 0 || n > closes_.size()) ? closes_.size() : n;
    size_t i = 0;
    while (i < limit && i < closes_.size()) {
      if (closes_[i].Wait(0)) {
        closes_.erase(closes_.begin() + i);
        if (limit > 0) --limit;
      } else {
        ++i;
      }
    }
  }

 private:
  PendingCloses() = default;
  std::mutex mu_;
  std::vector<chi::Future<clio::cte::core::DelTagTask>> closes_;
};

/** The type of seek to perform */
enum class SeekMode {
  kNone = -1,
  kSet = SEEK_SET,
  kCurrent = SEEK_CUR,
  kEnd = SEEK_END
};

/** A class to represent file system */
class Filesystem : public FilesystemIoClient {
public:
  AdapterType type_;

public:
  /** Constructor */
  explicit Filesystem(AdapterType type) : type_(type) {
    clio::cte::core::CLIO_CTE_CLIENT_INIT();
  }

  /**
   * Open \a path.
   *
   * The caller hands us a clio::-prefixed path (the prefix is the gate
   * IsPathTracked checks before calling here). We strip the prefix once
   * so every downstream consumer — backend RealOpen, CTE tag identity,
   * stat.path_ — sees the bare path.
   */
  File Open(AdapterStat &stat, const std::string &path) {
    File f;
    std::string clean_path = StripClioPrefix(path);
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    if (stat.adapter_mode_ == AdapterMode::kNone) {
      stat.adapter_mode_ = mdm->GetAdapterMode(clean_path);
    }
    RealOpen(f, stat, clean_path);
    if (!f.status_) {
      return f;
    }
    Open(stat, f, clean_path);
    return f;
  }

  /** open \a f File in \a path */
  void Open(AdapterStat &stat, File &f, const std::string &path) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;

    // Lazily reap up to 3 completed close-time AsyncDelTag futures
    // before issuing this open. Wait(0) is non-blocking so this is at
    // most a few FUTURE_COMPLETE flag reads per open.
    PendingCloses::Get().ReapN(3);

    std::shared_ptr<AdapterStat> exists = mdm->Find(f);
    if (!exists) {
      HLOG(kDebug, "File not opened before by adapter");
      stat.path_ = stdfs::absolute(path).string();
      stat.page_size_ = mdm->GetAdapterPageSize(path);

      // Async tag create: fire AsyncGetOrCreateTag and stash the future
      // on the stat. tag_id_ is unknown at this point; the first I/O
      // op (Read/Write/GetSize/...) will call AwaitPendingOpen to
      // resolve it. This removes the sync RPC from the open critical
      // path entirely.
      auto *cte_client = CLIO_CTE_CLIENT;
      stat.pending_open_fut_ = cte_client->AsyncGetOrCreateTag(stat.path_);
      stat.open_pending_ = true;
      stat.tag_id_ = clio::cte::core::TagId();  // sentinel; filled on first wait

      if (stat.hflags_.Any(CLIO_CTE_FS_TRUNC)) {
        stat.file_size_ = 0;
      } else {
        stat.file_size_ = GetBackendSize(stat.path_);
      }
      if (stat.hflags_.Any(CLIO_CTE_FS_APPEND)) {
        stat.st_ptr_ = std::numeric_limits<size_t>::max();
      } else {
        stat.st_ptr_ = 0;
      }
      auto stat_ptr = std::make_shared<AdapterStat>(stat);
      FilesystemIoClientState fs_ctx(&mdm->fs_mdm_, (void *)stat_ptr.get());
      HermesOpen(f, stat, fs_ctx);
      mdm->Create(f, stat_ptr);
    } else {
      HLOG(kDebug, "File already opened by adapter");
      exists->UpdateTime();
    }
  }

  /**
   * Block on the pending AsyncGetOrCreateTag for this stat (if any),
   * publish the resulting tag_id_, and (when not truncating) query
   * the CTE tag size so stat.file_size_ matches what's stored in CTE.
   *
   * Why the size query: writes via the CTE path don't touch the
   * backing FS (no RealWrite call), so GetBackendSize during Open
   * reports 0 for output files. Reads that hit EOF would then either
   * (a) return 0 immediately with my clamp, or (b) walk off the end
   * of the page space, triggering "GetBlob failed for page N" errors.
   * Filling file_size_ from CTE here fixes both.
   *
   * No-op (and zero-cost) when stat.open_pending_ is already false.
   */
  static void AwaitPendingOpen(AdapterStat &stat) {
    if (!stat.open_pending_) return;
    if (!stat.pending_open_fut_.Wait()) {
      HLOG(kError, "AwaitPendingOpen: AsyncGetOrCreateTag timed out for {}",
           stat.path_);
      stat.open_pending_ = false;
      return;
    }
    if (stat.pending_open_fut_->GetReturnCode() != 0) {
      HLOG(kError, "AwaitPendingOpen: GetOrCreateTag rc={} for {}",
           stat.pending_open_fut_->GetReturnCode(), stat.path_);
    }
    stat.tag_id_ = stat.pending_open_fut_->tag_id_;
    stat.open_pending_ = false;

    // Populate file_size_ from the CTE tag (unless we're truncating,
    // in which case the file is logically empty anyway).
    if (stat.adapter_mode_ != AdapterMode::kBypass &&
        !stat.hflags_.Any(CLIO_CTE_FS_TRUNC)) {
      auto *cte_client = CLIO_CTE_CLIENT;
      auto size_fut = cte_client->AsyncGetTagSize(stat.tag_id_);
      if (size_fut.Wait() && size_fut->GetReturnCode() == 0) {
        size_t cte_size = size_fut.get()->tag_size_;
        if (cte_size > 0) {
          stat.file_size_ = cte_size;
        }
      }
    }
    HLOG(kDebug,
         "AwaitPendingOpen: resolved tag_id={},{} file_size={} for {}",
         stat.tag_id_.major_, stat.tag_id_.minor_, stat.file_size_,
         stat.path_);
  }

private:
  /** Helper function to calculate page index from offset */
  static size_t CalculatePageIndex(size_t offset, size_t page_size) {
    return offset / page_size;
  }

  /** Helper function to calculate offset within a page */
  static size_t CalculatePageOffset(size_t offset, size_t page_size) {
    return offset % page_size;
  }

  /** Helper function to calculate remaining space in current page */
  static size_t CalculateRemainingPageSpace(size_t offset, size_t page_size) {
    size_t page_offset = CalculatePageOffset(offset, page_size);
    return page_size - page_offset;
  }

public:
  /** write */
  size_t Write(File &f, AdapterStat &stat, const void *ptr, size_t off,
               size_t total_size, IoStatus &io_status,
               FsIoOptions opts = FsIoOptions()) {
    (void)f;
    // Resolve a pending async open before issuing any CTE RPC.
    AwaitPendingOpen(stat);
    std::string filename = stat.path_;
    bool is_append = stat.st_ptr_ == std::numeric_limits<size_t>::max();

    // HLOG(kInfo,
    //       "Write called for filename: {}"
    //       " on offset: {}"
    //       " from position: {}"
    //       " and size: {}"
    //       " and adapter mode: {}",
    //       filename, off, stat.st_ptr_, total_size,
    //       AdapterModeConv::str(stat.adapter_mode_));
    if (stat.adapter_mode_ == AdapterMode::kBypass) {
      // Bypass mode is handled differently
      opts.backend_size_ = total_size;
      opts.backend_off_ = off;
      WriteBlob(filename, ptr, total_size, opts, io_status);
      if (!io_status.success_) {
        HLOG(kDebug, "Failed to write blob of size {} to backend",
              opts.backend_size_);
        return 0;
      }
      if (opts.DoSeek() && !is_append) {
        stat.st_ptr_ = off + total_size;
      }
      return total_size;
    }
    // CTE doesn't need Context objects

    if (is_append) {
      // TODO: Append operations not yet supported in CTE
      // Perform append
      HLOG(kWarning, "Append operations not yet supported in CTE, treating as "
                      "regular write");
      // Fallback to regular write at end of file
      off = stat.file_size_;
    }

    // Page-based CTE PutBlob with bounded-async dispatch.
    //
    // The original loop was strictly sync: issue one 1 MiB PutBlob,
    // block on Wait(), move on. Measured ceiling ~1 GB/s. clio_cte_bench
    // got 3.5 GB/s by pipelining 8 AsyncPutBlobs and reusing one SHM
    // buffer. This loop adopts the same pattern with a ring of slots:
    //
    //   - Pre-allocate `depth` SHM buffers (one per slot). Reusing them
    //     eliminates per-page AllocateBuffer + first-touch faults that
    //     Tag::PutBlob(const char*) incurs (measured ~340 us/MiB).
    //   - Issue AsyncPutBlob into the next slot without waiting.
    //   - Only wait when the next slot we want to dispatch into is
    //     still in-flight (ring full -> wait for the oldest).
    //   - Drain remaining in-flight slots at end of Write.
    //
    // Default depth = 16. Override at runtime with
    // CTE_ADAPTER_WRITE_DEPTH (env var).
    auto __wr_start = std::chrono::steady_clock::now();
    char __wr_label[96];
    snprintf(__wr_label, sizeof(__wr_label),
             "Write off=%zu size=%zu", off, total_size);
    {
      static const size_t kWriteDepth = []() -> size_t {
        const char *e = std::getenv("CTE_ADAPTER_WRITE_DEPTH");
        if (e && *e) {
          char *end = nullptr;
          unsigned long v = std::strtoul(e, &end, 10);
          if (end != e && v > 0 && v < 1024) {
            return static_cast<size_t>(v);
          }
        }
        return 16;
      }();

      size_t bytes_written = 0;
      size_t current_offset = off;
      const char *data_ptr = static_cast<const char *>(ptr);
      bool ok = true;

      clio::cte::core::Tag file_tag(stat.tag_id_);

      // Shrink ring to the number of pages actually needed (avoids
      // allocating 16 MiB of SHM for a 1-page write).
      size_t pages_needed = total_size == 0
          ? 0
          : (total_size + stat.page_size_ - 1) / stat.page_size_;
      size_t depth = std::min(kWriteDepth, pages_needed);
      if (depth == 0) depth = 1;

      struct Slot {
        ctp::ipc::FullPtr<char> shm;
        chi::Future<clio::cte::core::PutBlobTask> fut;
        bool in_flight = false;
      };
      auto *ipc_manager = CLIO_IPC;
      std::vector<Slot> slots(depth);
      size_t allocated = 0;
      for (; allocated < depth; ++allocated) {
        slots[allocated].shm =
            ipc_manager->AllocateBuffer(stat.page_size_);
        if (slots[allocated].shm.IsNull()) {
          HLOG(kError,
               "AllocateBuffer({} bytes) failed for write-ring slot {}",
               stat.page_size_, allocated);
          ok = false;
          break;
        }
      }

      size_t next_slot = 0;
      while (ok && bytes_written < total_size) {
        size_t page_index =
            CalculatePageIndex(current_offset, stat.page_size_);
        size_t page_offset =
            CalculatePageOffset(current_offset, stat.page_size_);
        size_t remaining_page_space =
            CalculateRemainingPageSpace(current_offset, stat.page_size_);
        size_t bytes_to_write =
            std::min(remaining_page_space, total_size - bytes_written);

        Slot &s = slots[next_slot];
        if (s.in_flight) {
          // Ring full -> reclaim the oldest slot by waiting on it.
          // This is the only place we block during the dispatch loop.
          s.fut.Wait();
          if (s.fut->GetReturnCode() != 0) {
            HLOG(kError,
                 "AsyncPutBlob (ring reclaim) failed slot={} page={} rc={}",
                 next_slot, page_index, s.fut->GetReturnCode());
            ok = false;
            break;
          }
          s.in_flight = false;
        }

        // Copy user data into this slot's pre-allocated SHM buffer.
        memcpy(s.shm.ptr_, data_ptr + bytes_written, bytes_to_write);

        // Dispatch async, do not wait.
        std::string blob_name = std::to_string(page_index);
        ctp::ipc::ShmPtr<> shm_ptr(s.shm.shm_);
        try {
          s.fut = file_tag.AsyncPutBlob(blob_name, shm_ptr,
                                        bytes_to_write, page_offset);
          s.in_flight = true;
        } catch (const std::exception &e) {
          HLOG(kError, "AsyncPutBlob threw page={}: {}", page_index,
               e.what());
          ok = false;
          break;
        }

        bytes_written += bytes_to_write;
        current_offset += bytes_to_write;
        next_slot = (next_slot + 1) % depth;
      }

      // Drain remaining in-flight slots regardless of `ok` so the
      // SHM buffers are safe to free.
      for (size_t i = 0; i < depth; ++i) {
        if (slots[i].in_flight) {
          slots[i].fut.Wait();
          if (slots[i].fut->GetReturnCode() != 0) {
            HLOG(kError, "AsyncPutBlob (final drain) failed slot={} rc={}",
                 i, slots[i].fut->GetReturnCode());
            ok = false;
          }
          slots[i].in_flight = false;
        }
      }

      // Return the SHM pool to the allocator.
      for (size_t i = 0; i < allocated; ++i) {
        ipc_manager->FreeBuffer(slots[i].shm);
      }

      if (!ok) {
        io_status.success_ = false;
        return bytes_written;
      }

      if (opts.DoSeek()) {
        stat.st_ptr_ = off + total_size;
      }
      // Track logical file size so a subsequent read in the same
      // process / on the same fd doesn't return EOF prematurely.
      if (off + total_size > stat.file_size_) {
        stat.file_size_ = off + total_size;
      }
    }
    auto __wr_end = std::chrono::steady_clock::now();
    auto __wr_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       __wr_end - __wr_start)
                       .count();
    double __wr_ms = __wr_ns / 1e6;
    double __wr_mbs = (__wr_ns > 0) ? (total_size * 1e3 / __wr_ns) : 0.0;
    HLOG(kInfo, "[Filesystem::Write] {} elapsed={:.2f}ms ({:.1f} MB/s)",
         __wr_label, __wr_ms, __wr_mbs);
    clio::cte::core::FlushPutBlobTiming(__wr_label);
    stat.UpdateTime();
    io_status.size_ = total_size;
    UpdateIoStatus(opts, io_status);

    HLOG(kDebug, "The size of file after write: {}", GetSize(f, stat));
    return total_size;
  }

  /** base read function */
  template <bool ASYNC>
  size_t BaseRead(File &f, AdapterStat &stat, void *ptr, size_t off,
                  size_t total_size, size_t req_id,
                  std::vector<GetBlobAsyncTask> &tasks, IoStatus &io_status,
                  FsIoOptions opts = FsIoOptions()) {
    (void)f;
    // Resolve a pending async open before issuing any CTE RPC.
    AwaitPendingOpen(stat);
    std::string filename = stat.path_;

    HLOG(kDebug,
          "Read called for filename: {}"
          " on offset: {}"
          " from position: {}"
          " and size: {}",
          stat.path_, off, stat.st_ptr_, total_size);

    // SEEK_END is not a valid read position
    if (off == std::numeric_limits<size_t>::max()) {
      io_status.size_ = 0;
      UpdateIoStatus(opts, io_status);
      return 0;
    }

    // Read bit must be set
    if (!stat.hflags_.Any(CLIO_CTE_FS_READ)) {
      io_status.size_ = 0;
      UpdateIoStatus(opts, io_status);
      return -1;
    }

    // Ensure the amount being read makes sense
    if (total_size == 0) {
      io_status.size_ = 0;
      UpdateIoStatus(opts, io_status);
      return 0;
    }

    if constexpr (!ASYNC) {
      if (stat.adapter_mode_ == AdapterMode::kBypass) {
        // Bypass mode is handled differently
        opts.backend_size_ = total_size;
        opts.backend_off_ = off;
        ReadBlob(filename, ptr, total_size, opts, io_status);
        if (!io_status.success_) {
          HLOG(kDebug, "Failed to read blob of size {} from backend",
                opts.backend_size_);
          return 0;
        }
        if (opts.DoSeek()) {
          stat.st_ptr_ = off + total_size;
        }
        return total_size;
      }
    }

    // CTE read operation - use page-based blob naming to match PutBlob
    if constexpr (ASYNC) {
      // TODO: Async read operations not yet fully supported in CTE adapter
      HLOG(kWarning,
            "Async read operations not yet fully supported, using sync read");
    }

    // Clamp the request to file_size_ so the page loop never asks for
    // pages past EOF. Callers (e.g. wfbench's `while fp.read(N): pass`)
    // routinely issue one extra read at EOF — the kernel/glibc returns
    // 0 from a real read syscall, but our page loop has no implicit
    // EOF awareness. Without clamping, an N-byte read at offset
    // file_size_ would issue ceil(N / page_size) GetBlob calls for
    // pages that were never written, each failing with rc != 0.
    if (off >= stat.file_size_) {
      io_status.size_ = 0;
      UpdateIoStatus(opts, io_status);
      return 0;
    }
    if (off + total_size > stat.file_size_) {
      total_size = stat.file_size_ - off;
    }

    // Use page-based CTE GetBlob operations with Tag API
    size_t bytes_read = 0;
    size_t current_offset = off;
    char *data_ptr = static_cast<char *>(ptr);

    // Create Tag object from stored TagId
    clio::cte::core::Tag file_tag(stat.tag_id_);

    while (bytes_read < total_size) {
      // Calculate current page index and offset within page
      size_t page_index = CalculatePageIndex(current_offset, stat.page_size_);
      size_t page_offset = CalculatePageOffset(current_offset, stat.page_size_);
      size_t remaining_page_space =
          CalculateRemainingPageSpace(current_offset, stat.page_size_);

      // Calculate how much to read from this page
      size_t bytes_to_read =
          std::min(remaining_page_space, total_size - bytes_read);

      // Generate blob name using stringified page index
      std::string blob_name = std::to_string(page_index);

      // Use Tag API GetBlob with raw char* (handles SHM allocation internally)
      try {
        file_tag.GetBlob(blob_name, data_ptr + bytes_read, bytes_to_read,
                         page_offset);
      } catch (const std::exception &e) {
        HLOG(kError, "Tag GetBlob failed for page {}: {}", page_index,
              e.what());
        io_status.success_ = false;
        return bytes_read;
      }

      // Update counters for next iteration
      bytes_read += bytes_to_read;
      current_offset += bytes_to_read;
    }

    size_t data_offset = bytes_read; // Total bytes read
    if (opts.DoSeek()) {
      stat.st_ptr_ = off + data_offset;
    }
    stat.UpdateTime();
    io_status.size_ = data_offset;
    UpdateIoStatus(opts, io_status);
    return data_offset;
  }

  /** read */
  size_t Read(File &f, AdapterStat &stat, void *ptr, size_t off,
              size_t total_size, IoStatus &io_status,
              FsIoOptions opts = FsIoOptions()) {
    std::vector<GetBlobAsyncTask> tasks;
    return BaseRead<false>(f, stat, ptr, off, total_size, 0, tasks, io_status,
                           opts);
  }

  /** write asynchronously */
  FsAsyncTask *AWrite(File &f, AdapterStat &stat, const void *ptr, size_t off,
                      size_t total_size, size_t req_id, IoStatus &io_status,
                      FsIoOptions opts = FsIoOptions()) {
    // Writes are completely async at this time
    FsAsyncTask *fstask = new FsAsyncTask();
    Write(f, stat, ptr, off, total_size, io_status, opts);
    fstask->io_status_.Copy(io_status);
    fstask->opts_ = opts;
    return fstask;
  }

  /** read asynchronously */
  FsAsyncTask *ARead(File &f, AdapterStat &stat, void *ptr, size_t off,
                     size_t total_size, size_t req_id, IoStatus &io_status,
                     FsIoOptions opts = FsIoOptions()) {
    FsAsyncTask *fstask = new FsAsyncTask();
    BaseRead<true>(f, stat, ptr, off, total_size, req_id, fstask->get_tasks_,
                   io_status, opts);
    fstask->io_status_ = io_status;
    fstask->opts_ = opts;
    return fstask;
  }

  /** wait for \a req_id request ID */
  size_t Wait(FsAsyncTask *fstask) {
    // chi::Future::Wait() blocks until the task completes and the
    // Future destructor cleans up the underlying task ptr — no
    // explicit CLIO_IPC->DelTask call needed any more.
    for (auto &fut : fstask->put_tasks_) {
      fut.Wait();
    }

    // Update I/O status for gets.
    if (!fstask->get_tasks_.empty()) {
      size_t get_size = 0;
      for (GetBlobAsyncTask &task : fstask->get_tasks_) {
        task.task_.Wait();
        // TODO: when the new GetBlobTask exposes the read size on
        // completion, use it here instead of the original request size.
        get_size += task.orig_size_;
      }
      fstask->io_status_.size_ = get_size;
      UpdateIoStatus(fstask->opts_, fstask->io_status_);
    }
    return 0;
  }

  /** wait for request IDs in \a req_id vector */
  void Wait(std::vector<FsAsyncTask *> &req_ids, std::vector<size_t> &ret) {
    for (auto &req_id : req_ids) {
      ret.emplace_back(Wait(req_id));
    }
  }

  /** seek */
  size_t Seek(File &f, AdapterStat &stat, SeekMode whence, off64_t offset) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    switch (whence) {
    case SeekMode::kSet: {
      stat.st_ptr_ = offset;
      break;
    }
    case SeekMode::kCurrent: {
      if (stat.st_ptr_ != std::numeric_limits<size_t>::max()) {
        stat.st_ptr_ = (off64_t)stat.st_ptr_ + offset;
        offset = stat.st_ptr_;
      } else {
        stat.st_ptr_ = (off64_t)stat.file_size_ + offset;
        offset = stat.st_ptr_;
      }
      break;
    }
    case SeekMode::kEnd: {
      if (offset == 0) {
        stat.st_ptr_ = std::numeric_limits<size_t>::max();
        offset = stat.file_size_;
      } else {
        stat.st_ptr_ = (off64_t)stat.file_size_ + offset;
        offset = stat.st_ptr_;
      }
      break;
    }
    default: {
      HLOG(kError, "Invalid seek mode");
      return (size_t)-1;
    }
    }
    mdm->Update(f, stat);
    return offset;
  }

  /** file size */
  size_t GetSize(File &f, AdapterStat &stat) {
    (void)f;
    // Resolve a pending async open before issuing the CTE size RPC.
    AwaitPendingOpen(stat);
    if (stat.adapter_mode_ != AdapterMode::kBypass) {
      // clio_cte_core dropped the synchronous GetTagSize wrapper; the
      // current API is AsyncGetTagSize + Future.Wait(), with the result
      // surfacing on the task's tag_size_ field via the Future's
      // task_ptr_.
      auto *cte_client = CLIO_CTE_CLIENT;
      auto fut = cte_client->AsyncGetTagSize(stat.tag_id_);
      fut.Wait();
      size_t cte_tag_size = fut.get()->tag_size_;

      HLOG(
          kDebug,
          "GetSize: queried CTE for tag_id={},{}, got size={}, cached_size={}",
          stat.tag_id_.major_, stat.tag_id_.minor_, cte_tag_size,
          stat.file_size_);

      stat.file_size_ = cte_tag_size;
      return cte_tag_size;
    } else {
      return stdfs::file_size(stat.path_);
    }
  }

  /** tell */
  size_t Tell(File &f, AdapterStat &stat) {
    (void)f;
    if (stat.st_ptr_ != std::numeric_limits<size_t>::max()) {
      return stat.st_ptr_;
    } else {
      return stat.file_size_;
    }
  }

  /** sync */
  int Sync(File &f, AdapterStat &stat) {
    (void)f;
    (void)stat;
    // CTE sync operations would be handled by the runtime
    // For now, no explicit sync needed
    return 0;
  }

  /** truncate */
  int Truncate(File &f, AdapterStat &stat, size_t new_size) {
    // hapi::Bucket &bkt = stat.bkt_id_;
    // TODO(llogan)
    return 0;
  }

  /** close */
  int Close(File &f, AdapterStat &stat) {
    Sync(f, stat);
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    FilesystemIoClientState fs_ctx(&mdm->fs_mdm_, (void *)&stat);
    HermesClose(f, stat, fs_ctx);
    RealClose(f, stat);
    mdm->Delete(stat.path_, f);
    if (stat.amode_ & MPI_MODE_DELETE_ON_CLOSE) {
      Remove(stat.path_);
    }
    // CTE doesn't require explicit flush operations
    // Runtime handles persistence automatically
    return 0;
  }

  /** remove */
  int Remove(const std::string &pathname) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    int ret = RealRemove(pathname);

    // CTE tag cleanup — async fire-and-forget. The AsyncDelTag future
    // is parked on the process-wide PendingCloses queue; subsequent
    // opens lazily poll the first few entries with Future::Wait(0)
    // (non-blocking) to reap completed ones. This keeps the close-time
    // round-trip off the caller's critical path.
    std::string canon_path = stdfs::absolute(pathname).string();
    auto *cte_client = CLIO_CTE_CLIENT;
    PendingCloses::Get().Push(cte_client->AsyncDelTag(canon_path));
    HLOG(kDebug, "Queued CTE tag delete for file: {}", pathname);

    // Destroy all file descriptors
    std::list<File> *filesp = mdm->Find(pathname);
    if (filesp == nullptr) {
      return ret;
    }
    HLOG(kDebug, "Destroying the file descriptors: {}", pathname);
    std::list<File> files = *filesp;
    for (File &f : files) {
      std::shared_ptr<AdapterStat> stat = mdm->Find(f);
      if (stat == nullptr) {
        continue;
      }
      FilesystemIoClientState fs_ctx(&mdm->fs_mdm_, (void *)&stat);
      HermesClose(f, *stat, fs_ctx);
      RealClose(f, *stat);
      mdm->Delete(stat->path_, f);
      if (stat->adapter_mode_ == AdapterMode::kScratch) {
        ret = 0;
      }
    }
    return ret;
  }

  /**
   * I/O APIs which seek based on the internal AdapterStat st_ptr,
   * instead of taking an offset as input.
   */

public:
  /** write */
  size_t Write(File &f, AdapterStat &stat, const void *ptr, size_t total_size,
               IoStatus &io_status, FsIoOptions opts) {
    size_t off = stat.st_ptr_;
    return Write(f, stat, ptr, off, total_size, io_status, opts);
  }

  /** read */
  size_t Read(File &f, AdapterStat &stat, void *ptr, size_t total_size,
              IoStatus &io_status, FsIoOptions opts) {
    size_t off = stat.st_ptr_;
    return Read(f, stat, ptr, off, total_size, io_status, opts);
  }

  /** write asynchronously */
  FsAsyncTask *AWrite(File &f, AdapterStat &stat, const void *ptr,
                      size_t total_size, size_t req_id, IoStatus &io_status,
                      FsIoOptions opts) {
    size_t off = stat.st_ptr_;
    return AWrite(f, stat, ptr, off, total_size, req_id, io_status, opts);
  }

  /** read asynchronously */
  FsAsyncTask *ARead(File &f, AdapterStat &stat, void *ptr, size_t total_size,
                     size_t req_id, IoStatus &io_status, FsIoOptions opts) {
    size_t off = stat.st_ptr_;
    return ARead(f, stat, ptr, off, total_size, req_id, io_status, opts);
  }

  /**
   * Locates the AdapterStat data structure internally, and
   * call the underlying APIs which take AdapterStat as input.
   */

public:
  /** write */
  size_t Write(File &f, bool &stat_exists, const void *ptr, size_t total_size,
               IoStatus &io_status, FsIoOptions opts = FsIoOptions()) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    return Write(f, *stat, ptr, total_size, io_status, opts);
  }

  /** read */
  size_t Read(File &f, bool &stat_exists, void *ptr, size_t total_size,
              IoStatus &io_status, FsIoOptions opts = FsIoOptions()) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    return Read(f, *stat, ptr, total_size, io_status, opts);
  }

  /** write \a off offset */
  size_t Write(File &f, bool &stat_exists, const void *ptr, size_t off,
               size_t total_size, IoStatus &io_status,
               FsIoOptions opts = FsIoOptions()) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    opts.UnsetSeek();
    return Write(f, *stat, ptr, off, total_size, io_status, opts);
  }

  /** read \a off offset */
  size_t Read(File &f, bool &stat_exists, void *ptr, size_t off,
              size_t total_size, IoStatus &io_status,
              FsIoOptions opts = FsIoOptions()) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    opts.UnsetSeek();
    return Read(f, *stat, ptr, off, total_size, io_status, opts);
  }

  /** write asynchronously */
  FsAsyncTask *
  AWrite(File &f, bool &stat_exists, const void *ptr, size_t total_size,
         size_t req_id,
         std::vector<ctp::ipc::FullPtr<clio::cte::core::PutBlobTask>> &tasks,
         IoStatus &io_status, FsIoOptions opts) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    return AWrite(f, *stat, ptr, total_size, req_id, io_status, opts);
  }

  /** read asynchronously */
  FsAsyncTask *ARead(File &f, bool &stat_exists, void *ptr, size_t total_size,
                     size_t req_id, IoStatus &io_status, FsIoOptions opts) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    return ARead(f, *stat, ptr, total_size, req_id, io_status, opts);
  }

  /** write \a off offset asynchronously */
  FsAsyncTask *AWrite(File &f, bool &stat_exists, const void *ptr, size_t off,
                      size_t total_size, size_t req_id, IoStatus &io_status,
                      FsIoOptions opts) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    opts.UnsetSeek();
    return AWrite(f, *stat, ptr, off, total_size, req_id, io_status, opts);
  }

  /** read \a off offset asynchronously */
  FsAsyncTask *ARead(File &f, bool &stat_exists, void *ptr, size_t off,
                     size_t total_size, size_t req_id, IoStatus &io_status,
                     FsIoOptions opts) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return 0;
    }
    stat_exists = true;
    opts.UnsetSeek();
    return ARead(f, *stat, ptr, off, total_size, req_id, io_status, opts);
  }

  /** seek */
  size_t Seek(File &f, bool &stat_exists, SeekMode whence, size_t offset) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return (size_t)-1;
    }
    stat_exists = true;
    return Seek(f, *stat, whence, offset);
  }

  /** file sizes */
  size_t GetSize(File &f, bool &stat_exists) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return (size_t)-1;
    }
    stat_exists = true;
    return GetSize(f, *stat);
  }

  /** tell */
  size_t Tell(File &f, bool &stat_exists) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return (size_t)-1;
    }
    stat_exists = true;
    return Tell(f, *stat);
  }

  /** sync */
  int Sync(File &f, bool &stat_exists) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return -1;
    }
    stat_exists = true;
    return Sync(f, *stat);
  }

  /** truncate */
  int Truncate(File &f, bool &stat_exists, size_t new_size) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return -1;
    }
    stat_exists = true;
    return Truncate(f, *stat, new_size);
  }

  /** close */
  int Close(File &f, bool &stat_exists) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto stat = mdm->Find(f);
    if (!stat) {
      stat_exists = false;
      return -1;
    }
    stat_exists = true;
    return Close(f, *stat);
  }

public:
  /**
   * A path is tracked iff it carries the "clio::" prefix.
   *
   * No regex, no include/exclude lists, no YAML config: the user opts in
   * per call by typing the prefix. The CTE manager must also be live;
   * if it hasn't initialized yet we pass through to the real syscall.
   */
  static bool IsPathTracked(const std::string &path) {
    if (!HasClioPrefix(path)) {
      return false;
    }
    auto *cte_manager = CTE_MANAGER;
    if (cte_manager != nullptr && !cte_manager->IsInitialized()) {
      return false;
    }
    return true;
  }
};

} // namespace clio::cae

#endif // CLIO_CTE_ADAPTER_FILESYSTEM_FILESYSTEM_H_

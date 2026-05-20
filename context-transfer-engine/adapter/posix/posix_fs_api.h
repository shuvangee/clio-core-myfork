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

#ifndef CLIO_CTE_ADAPTER_POSIX_NATIVE_H_
#define CLIO_CTE_ADAPTER_POSIX_NATIVE_H_

#include <memory>

#include "adapter/filesystem/filesystem.h"
#include "adapter/filesystem/filesystem_mdm.h"
#include "posix_api.h"
#include "clio_cte/core/content_transfer_engine.h"

namespace clio::cae {

/** A class to represent POSIX IO file system */
class PosixFs : public clio::cae::Filesystem {
public:
  CLIO_CTE_POSIX_API_T real_api_; /**< pointer to real APIs */

public:
  PosixFs() : Filesystem(AdapterType::kPosix) { real_api_ = CLIO_CTE_POSIX_API; }

  /**
   * Fill a struct stat / stat64 for a clio:: file.
   *
   * Apps inspect way more than just st_size: cp/rsync read st_blocks
   * for sparse-file detection, du wants st_blksize, find -type checks
   * the file-type bits in st_mode, anything tracking change-detection
   * looks at st_dev+st_ino as a unique-id pair. Zero the whole struct
   * first, then fill every field we have a sensible answer for.
   */
  template <typename StatT> int Stat(File &f, StatT *buf) {
    auto mdm = CLIO_CTE_FS_METADATA_MANAGER;
    auto existing = mdm->Find(f);
    if (!existing) {
      errno = EBADF;
      HLOG(kError, "File with descriptor {} does not exist in CTE",
            f.hermes_fd_);
      return -1;
    }
    AdapterStat &astat = *existing;
    FillStat(*buf, astat, f);
    errno = 0;
    return 0;
  }

  template <typename StatT> int Stat(const char *__filename, StatT *buf) {
    bool stat_exists;
    AdapterStat stat;
    stat.flags_ = O_RDONLY;
    stat.st_mode_ = 0;
    File f = Open(stat, __filename);
    if (!f.status_) {
      memset(buf, 0, sizeof(StatT));
      errno = ENOENT;
      return -1;
    }
    int result = Stat(f, buf);
    Close(f, stat_exists);
    return result;
  }

private:
  /** Compute a stable 64-bit inode number from the bare backend path. */
  static uint64_t SyntheticInode(const std::string &path) {
    // std::hash gives us something stable per-process; combine high+low
    // halves to spread bits across a 64-bit ino_t.
    size_t h = std::hash<std::string>{}(path);
    return static_cast<uint64_t>(h) ^
           (static_cast<uint64_t>(h) << 32);
  }

  /** Synthetic device number — same constant for every clio:: file. */
  static constexpr dev_t kClioStDev = static_cast<dev_t>(0xC110);

  template <typename StatT>
  void FillStat(StatT &buf, AdapterStat &astat, File &f) {
    memset(&buf, 0, sizeof(StatT));
    buf.st_dev = kClioStDev;
    buf.st_ino = SyntheticInode(astat.path_);
    buf.st_mode = S_IFREG | 0644;
    buf.st_nlink = 1;
    buf.st_uid = CTP_SYSTEM_INFO->uid_;
    buf.st_gid = CTP_SYSTEM_INFO->gid_;
    buf.st_rdev = 0;
    size_t size = GetSize(f, astat);
    buf.st_size = static_cast<off_t>(size);
    buf.st_blksize = static_cast<blksize_t>(kMdmAdapterPageSize);
    // POSIX defines st_blocks as the count of 512-byte units allocated,
    // not the count of page-sized blocks. Round up so a non-empty file
    // never reports 0.
    buf.st_blocks = static_cast<blkcnt_t>((size + 511) / 512);
    buf.st_atime = astat.st_atim_.tv_sec;
    buf.st_mtime = astat.st_mtim_.tv_sec;
    buf.st_ctime = astat.st_ctim_.tv_sec;
  }

public:

  /**
   * Whether \a fd was handed out by CTE.
   *
   * CTE-issued fds start at 8192 to avoid colliding with the kernel's
   * fd-numbering space. If CTE isn't initialized we can't have issued
   * anything so it's a cheap "no". Otherwise we look it up in the MDM.
   */
  static bool IsFdTracked(int fd, std::shared_ptr<AdapterStat> &stat) {
    if (fd < 8192) {
      return false;
    }
    auto *cte_manager = CTE_MANAGER;
    if (cte_manager != nullptr && !cte_manager->IsInitialized()) {
      return false;
    }
    clio::cae::File f;
    f.hermes_fd_ = fd;
    stat = CLIO_CTE_FS_METADATA_MANAGER->Find(f);
    return stat != nullptr;
  }

  /** Whether or not \a fd FILE DESCRIPTOR was generated by Clio */
  static bool IsFdTracked(int fd) {
    std::shared_ptr<AdapterStat> stat;
    return IsFdTracked(fd, stat);
  }

public:
  /** Allocate an fd for the file f */
  void RealOpen(File &f, AdapterStat &stat, const std::string &path) override {
    // Check the first two bits for read/write mode
    switch (stat.flags_ & O_ACCMODE) {
    case O_RDONLY:
      stat.hflags_.SetBits(CLIO_CTE_FS_READ);
      break;
    case O_WRONLY:
      stat.hflags_.SetBits(CLIO_CTE_FS_WRITE);
      break;
    case O_RDWR:
      stat.hflags_.SetBits(CLIO_CTE_FS_READ | CLIO_CTE_FS_WRITE);
      break;
    }
    if (stat.flags_ & O_APPEND) {
      stat.hflags_.SetBits(CLIO_CTE_FS_APPEND);
    }
    if (stat.flags_ & O_CREAT || stat.flags_ & O_TMPFILE) {
      stat.hflags_.SetBits(CLIO_CTE_FS_CREATE);
    }
    if (stat.flags_ & O_TRUNC) {
      stat.hflags_.SetBits(CLIO_CTE_FS_TRUNC);
    }

    if (stat.hflags_.Any(CLIO_CTE_FS_CREATE)) {
      if (stat.adapter_mode_ != AdapterMode::kScratch) {
        stat.fd_ = real_api_->open(path.c_str(), stat.flags_, stat.st_mode_);
      }
    } else {
      stat.fd_ = real_api_->open(path.c_str(), stat.flags_);
    }

    if (stat.fd_ >= 0) {
      stat.hflags_.SetBits(CLIO_CTE_FS_EXISTS);
    }
    if (stat.fd_ < 0 && stat.adapter_mode_ != AdapterMode::kScratch) {
      f.status_ = false;
    }
  }

  /**
   * Called after real open. Allocates the Clio representation of
   * identifying file information, such as a clio file descriptor
   * and clio file handler. These are not the same as POSIX file
   * descriptor and STDIO file handler.
   * */
  void HermesOpen(File &f, const AdapterStat &stat,
                  FilesystemIoClientState &fs_mdm) override {
    f.hermes_fd_ = fs_mdm.mdm_->AllocateFd();
  }

  /** Synchronize \a file FILE f */
  int RealSync(const File &f, const AdapterStat &stat) override {
    (void)f;
    if (stat.adapter_mode_ == AdapterMode::kScratch && stat.fd_ == -1) {
      return 0;
    }
    return real_api_->fsync(stat.fd_);
  }

  /** Close \a file FILE f */
  int RealClose(const File &f, AdapterStat &stat) override {
    (void)f;
    if (stat.adapter_mode_ == AdapterMode::kScratch && stat.fd_ == -1) {
      return 0;
    }
    return real_api_->close(stat.fd_);
  }

  /**
   * Called before RealClose. Releases information provisioned during
   * the allocation phase.
   * */
  void HermesClose(File &f, const AdapterStat &stat,
                   FilesystemIoClientState &fs_mdm) override {
    fs_mdm.mdm_->ReleaseFd(f.hermes_fd_);
  }

  /** Remove \a file FILE f */
  int RealRemove(const std::string &path) override {
    return real_api_->remove(path.c_str());
  }

  /** Get initial statistics from the backend */
  size_t GetBackendSize(const std::string &bkt_name) override {
    size_t true_size = 0;
    std::string filename = bkt_name;
    int fd = real_api_->open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
      return 0;
    }
    struct stat buf;
    real_api_->fstat(fd, &buf);
    true_size = buf.st_size;
    real_api_->close(fd);

    HLOG(kDebug, "The size of the file {} on disk is {}", filename, true_size);
    return true_size;
  }

  /** Write blob to backend */
  void WriteBlob(const std::string &bkt_name, const void* data, size_t size,
                 const FsIoOptions &opts, IoStatus &status) override {
    (void)opts;
    status.success_ = true;
    HLOG(kDebug,
          "Writing to file: {}"
          " offset: {}"
          " size: {}",
          bkt_name, opts.backend_off_, size);
    int fd = real_api_->open(bkt_name.c_str(), O_RDWR | O_CREAT);
    if (fd < 0) {
      status.size_ = 0;
      status.success_ = false;
      return;
    }
    status.size_ = real_api_->pwrite(fd, data, size, opts.backend_off_);
    if (status.size_ != size) {
      status.success_ = false;
    }
    real_api_->close(fd);
  }

  /** Read blob from the backend */
  void ReadBlob(const std::string &bkt_name, void* data, size_t size,
                const FsIoOptions &opts, IoStatus &status) override {
    (void)opts;
    status.success_ = true;
    HLOG(kDebug,
          "Reading from file: {}"
          " offset: {}"
          " size: {}",
          bkt_name, opts.backend_off_, size);
    int fd = real_api_->open(bkt_name.c_str(), O_RDONLY);
    if (fd < 0) {
      status.size_ = 0;
      status.success_ = false;
      return;
    }
    status.size_ = real_api_->pread(fd, data, size, opts.backend_off_);
    if (status.size_ != size) {
      status.success_ = false;
    }
    real_api_->close(fd);
  }

  void UpdateIoStatus(const FsIoOptions &opts, IoStatus &status) override {
    (void)opts;
    (void)status;
  }
};

} // namespace clio::cae

// Global pointer-based singleton
#include "clio_ctp/util/singleton.h"

namespace clio::cae {
CTP_DEFINE_GLOBAL_PTR_VAR_H(PosixFs, g_posix_fs);
}

/** Simplify access to the stateless PosixFs Singleton */
#define CLIO_CTE_POSIX_FS (CTP_GET_GLOBAL_PTR_VAR(clio::cae::PosixFs, clio::cae::g_posix_fs))
#define CLIO_CTE_POSIX_FS_T clio::cae::PosixFs *

#endif // CLIO_CTE_ADAPTER_POSIX_NATIVE_H_

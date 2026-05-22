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

#ifndef CTP_SHM_INCLUDE_HSHM_SHM_IO_IOCP_IO_H_
#define CTP_SHM_INCLUDE_HSHM_SHM_IO_IOCP_IO_H_

#ifdef _WIN32

#include "async_io.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef Yield
#undef Yield
#endif
#ifdef SendMessage
#undef SendMessage
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace ctp {

/**
 * Windows IOCP-based AsyncIO. Counterpart of LinuxAioAsyncIO; uses
 * CreateFile(FILE_FLAG_OVERLAPPED), ReadFile/WriteFile with per-op
 * OVERLAPPED structs, and GetQueuedCompletionStatus(Ex) to harvest
 * completions.
 *
 * Two handles per file:
 *   - regular_fd_: normal buffered I/O
 *   - direct_fd_:  FILE_FLAG_NO_BUFFERING (Windows analogue of O_DIRECT).
 *     Used when both buffer and length are sector-aligned.
 *
 * Both handles are associated with the same I/O completion port, keyed by
 * a single completion key so the harvest loop doesn't care which fd ran
 * the op.
 */
class IocpAsyncIO : public AsyncIO {
 public:
  explicit IocpAsyncIO(uint32_t io_depth)
      : iocp_(nullptr),
        regular_fd_(INVALID_HANDLE_VALUE),
        direct_fd_(INVALID_HANDLE_VALUE),
        io_depth_(io_depth),
        next_token_(1) {}

  ~IocpAsyncIO() override { Close(); }

  bool Open(const std::string &path, int flags, mode_t mode) override {
    (void)mode;  // Windows file mode bits live in the security descriptor;
                 // POSIX mode is ignored here.
    std::lock_guard<std::mutex> lock(mutex_);

    DWORD access = 0;
    // _O_RDWR / _O_WRONLY / _O_RDONLY values come from MSVC's <fcntl.h>.
    if ((flags & _O_RDWR) == _O_RDWR) {
      access = GENERIC_READ | GENERIC_WRITE;
    } else if ((flags & _O_WRONLY) == _O_WRONLY) {
      access = GENERIC_WRITE;
    } else {
      access = GENERIC_READ;
    }
    DWORD creation = (flags & _O_CREAT) ? OPEN_ALWAYS : OPEN_EXISTING;
    if (flags & _O_TRUNC) creation = CREATE_ALWAYS;

    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;

    // Regular (buffered) handle.
    regular_fd_ = ::CreateFileA(path.c_str(), access, share, nullptr,
                                 creation, FILE_FLAG_OVERLAPPED, nullptr);
    if (regular_fd_ == INVALID_HANDLE_VALUE) {
      return false;
    }

    // Direct (unbuffered) handle — best-effort. Requires writes/reads to be
    // sector-aligned. If creation fails (e.g. compressed FS), we fall back
    // to using regular_fd_ for everything.
    direct_fd_ = ::CreateFileA(
        path.c_str(), access, share, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    // direct_fd_ may be INVALID_HANDLE_VALUE — that's fine.

    // Single completion port shared by both handles. The key is just a
    // sentinel (we route completions by OVERLAPPED pointer, not key).
    iocp_ = ::CreateIoCompletionPort(regular_fd_, nullptr,
                                      /*key=*/kCompletionKey,
                                      /*concurrency=*/0);
    if (!iocp_) {
      ::CloseHandle(regular_fd_);
      regular_fd_ = INVALID_HANDLE_VALUE;
      if (direct_fd_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(direct_fd_);
        direct_fd_ = INVALID_HANDLE_VALUE;
      }
      return false;
    }
    if (direct_fd_ != INVALID_HANDLE_VALUE) {
      if (!::CreateIoCompletionPort(direct_fd_, iocp_, kCompletionKey, 0)) {
        // Couldn't attach the direct handle — drop it and continue with
        // just the regular handle.
        ::CloseHandle(direct_fd_);
        direct_fd_ = INVALID_HANDLE_VALUE;
      }
    }

    return true;
  }

  ssize_t GetFileSize() const override {
    HANDLE h = (regular_fd_ != INVALID_HANDLE_VALUE) ? regular_fd_ : direct_fd_;
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    if (!::GetFileSizeEx(h, &sz)) return -1;
    return static_cast<ssize_t>(sz.QuadPart);
  }

  bool Truncate(size_t size) override {
    HANDLE h = (regular_fd_ != INVALID_HANDLE_VALUE) ? regular_fd_ : direct_fd_;
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
    if (!::SetEndOfFile(h)) return false;
    return true;
  }

  IoToken Write(void *buffer, size_t size, off_t offset) override {
    return SubmitIO(buffer, size, offset, /*is_write=*/true);
  }

  IoToken Read(void *buffer, size_t size, off_t offset) override {
    return SubmitIO(buffer, size, offset, /*is_write=*/false);
  }

  bool IsComplete(IoToken token, IoResult &result) override {
    std::lock_guard<std::mutex> lock(mutex_);

    auto already = completed_.find(token);
    if (already != completed_.end()) {
      result = already->second;
      completed_.erase(already);
      in_flight_.erase(token);
      return true;
    }

    HarvestCompletions();

    auto it = completed_.find(token);
    if (it != completed_.end()) {
      result = it->second;
      completed_.erase(it);
      in_flight_.erase(token);
      return true;
    }
    return false;
  }

  void Close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    // Cancel any in-flight I/O on each handle. CancelIoEx returns FALSE if
    // there's nothing to cancel — benign.
    if (regular_fd_ != INVALID_HANDLE_VALUE) {
      ::CancelIoEx(regular_fd_, nullptr);
      ::CloseHandle(regular_fd_);
      regular_fd_ = INVALID_HANDLE_VALUE;
    }
    if (direct_fd_ != INVALID_HANDLE_VALUE) {
      ::CancelIoEx(direct_fd_, nullptr);
      ::CloseHandle(direct_fd_);
      direct_fd_ = INVALID_HANDLE_VALUE;
    }
    // Drain remaining completions so the per-op heap allocations don't leak.
    if (iocp_) {
      DrainAllCompletions();
      ::CloseHandle(iocp_);
      iocp_ = nullptr;
    }
    in_flight_.clear();
    completed_.clear();
  }

  /** Windows has no eventfd; the IOCP handle itself is the wait object.
   *  Returning -1 matches the AsyncIO contract for "not available". */
  int GetEventFd() const override { return -1; }

  /** Windows-specific: hand the IOCP HANDLE to the caller if they want to
   *  wait on it directly (e.g. wire into a worker's WaitForMultipleObjects
   *  loop). Returns nullptr if Open() hasn't run. */
  HANDLE GetIocpHandle() const { return iocp_; }

 private:
  static constexpr ULONG_PTR kCompletionKey = 1;

  /** Per-operation tracking. OVERLAPPED MUST be the first field so a
   *  pointer-to-IoOp is a valid pointer-to-OVERLAPPED for Win32. */
  struct IoOp {
    OVERLAPPED overlapped;
    IoToken token;
    bool is_write;
  };

  IoToken SubmitIO(void *buffer, size_t size, off_t offset, bool is_write) {
    std::lock_guard<std::mutex> lock(mutex_);

    HANDLE h = SelectHandle(buffer, size);
    if (h == INVALID_HANDLE_VALUE) return kInvalidIoToken;

    IoToken token = next_token_.fetch_add(1, std::memory_order_relaxed);

    // Allocate the IoOp on the heap so the OVERLAPPED stays valid until
    // the kernel posts the completion.
    auto *op = new IoOp{};
    op->overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
    op->overlapped.OffsetHigh =
        static_cast<DWORD>((static_cast<uint64_t>(offset) >> 32) & 0xFFFFFFFF);
    op->token = token;
    op->is_write = is_write;

    BOOL ok;
    DWORD bytes_immediate = 0;
    if (is_write) {
      ok = ::WriteFile(h, buffer, static_cast<DWORD>(size),
                        &bytes_immediate, &op->overlapped);
    } else {
      ok = ::ReadFile(h, buffer, static_cast<DWORD>(size),
                       &bytes_immediate, &op->overlapped);
    }

    if (!ok) {
      DWORD err = ::GetLastError();
      if (err != ERROR_IO_PENDING) {
        // Synchronous failure — completion port won't fire for this op, so
        // record it as failed right now and return the token (the caller
        // can still observe the error via IsComplete).
        IoResult res;
        res.bytes_transferred = -1;
        res.error_code = static_cast<int>(err);
        completed_[token] = res;
        delete op;
        in_flight_.insert(token);
        return token;
      }
    } else {
      // Completed inline. The completion packet is STILL queued to the IOCP
      // unless we set the handle's FILE_SKIP_COMPLETION_PORT_ON_SUCCESS bit
      // (we don't), so the harvest loop will pick it up later.
    }

    in_flight_.insert(token);
    return token;
  }

  HANDLE SelectHandle(void *buffer, size_t size) const {
    // Use direct (unbuffered) handle when buffer + size are sector-aligned.
    // The disk sector size could in principle differ from 4096, but querying
    // it per-op via DeviceIoControl(FSCTL_GET_NTFS_VOLUME_DATA) is overkill;
    // 4 KiB is correct for every modern NTFS/ReFS volume.
    if (direct_fd_ != INVALID_HANDLE_VALUE &&
        (reinterpret_cast<uintptr_t>(buffer) % 4096 == 0) &&
        (size % 4096 == 0)) {
      return direct_fd_;
    }
    return regular_fd_;
  }

  void HarvestCompletions() {
    // Called with mutex_ held.
    if (!iocp_) return;
    const ULONG kMax = 32;
    OVERLAPPED_ENTRY entries[kMax];
    ULONG removed = 0;
    if (!::GetQueuedCompletionStatusEx(iocp_, entries, kMax, &removed,
                                        /*timeout=*/0,
                                        /*alertable=*/FALSE)) {
      // No completions available, or a real error. Either way nothing to do.
      return;
    }
    for (ULONG i = 0; i < removed; ++i) {
      auto *op = reinterpret_cast<IoOp *>(entries[i].lpOverlapped);
      if (!op) continue;
      IoResult res;
      res.bytes_transferred =
          static_cast<ssize_t>(entries[i].dwNumberOfBytesTransferred);
      // GetOverlappedResult would surface the I/O-level status; we
      // approximate by treating zero-byte completions on a write as success
      // (it can happen for an empty op) and any explicit error code via
      // op->overlapped.Internal (NTSTATUS, lower 32 bits).
      LONG status = static_cast<LONG>(op->overlapped.Internal);
      if (status != 0) {
        res.bytes_transferred = -1;
        res.error_code = static_cast<int>(status);
      } else {
        res.error_code = 0;
      }
      completed_[op->token] = res;
      delete op;
    }
  }

  /** Called from Close() to ensure no IoOp allocations leak after the
   *  user-facing handles are gone. */
  void DrainAllCompletions() {
    const ULONG kMax = 64;
    OVERLAPPED_ENTRY entries[kMax];
    ULONG removed = 0;
    // Pull everything that's already queued. We don't block here.
    while (::GetQueuedCompletionStatusEx(iocp_, entries, kMax, &removed,
                                          /*timeout=*/0,
                                          /*alertable=*/FALSE) &&
           removed > 0) {
      for (ULONG i = 0; i < removed; ++i) {
        delete reinterpret_cast<IoOp *>(entries[i].lpOverlapped);
      }
    }
  }

  HANDLE iocp_;
  HANDLE regular_fd_;
  HANDLE direct_fd_;
  uint32_t io_depth_;
  std::atomic<IoToken> next_token_;
  std::mutex mutex_;
  std::unordered_set<IoToken> in_flight_;
  std::unordered_map<IoToken, IoResult> completed_;
};

}  // namespace ctp

#endif  // _WIN32

#endif  // CTP_SHM_INCLUDE_HSHM_SHM_IO_IOCP_IO_H_

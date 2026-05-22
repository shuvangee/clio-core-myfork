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

#ifndef CTP_TIMER_H
#define CTP_TIMER_H

#include <time.h>
#ifdef _WIN32
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
#endif

#include <chrono>
#include <functional>
#include <vector>

#include "clio_ctp/constants/macros.h"
// #include "clio_ctp/data_structures/internal/shm_archive.h"
#include "singleton.h"

namespace ctp {

template <typename T>
class TimepointBase {
 public:
  std::chrono::time_point<T> start_;

 public:
  CTP_INLINE_CROSS_FUN void Now() {
#if CTP_IS_HOST
    start_ = T::now();
#endif
  }
  CTP_INLINE_CROSS_FUN double GetNsecFromStart(TimepointBase &now) const {
#if CTP_IS_HOST
    double elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         now.start_ - start_)
                         .count();
    return elapsed;
#else
    return 0;
#endif
  }
  CTP_INLINE_CROSS_FUN double GetUsecFromStart(TimepointBase &now) const {
    return GetNsecFromStart(now) / 1000;
  }
  CTP_INLINE_CROSS_FUN double GetMsecFromStart(TimepointBase &now) const {
    return GetNsecFromStart(now) / 1000000;
  }
  CTP_INLINE_CROSS_FUN double GetSecFromStart(TimepointBase &now) const {
    return GetNsecFromStart(now) / 1000000000;
  }
  CTP_INLINE_CROSS_FUN double GetNsecFromStart() const {
#if CTP_IS_HOST
    std::chrono::time_point<T> end = T::now();
    double elapsed =
        (double)std::chrono::duration_cast<std::chrono::nanoseconds>(end -
                                                                     start_)
            .count();
    return elapsed;
#else
    return 0;
#endif
  }
  CTP_INLINE_CROSS_FUN double GetUsecFromStart() const {
    return GetNsecFromStart() / 1000;
  }
  CTP_INLINE_CROSS_FUN double GetMsecFromStart() const {
    return GetNsecFromStart() / 1000000;
  }
  CTP_INLINE_CROSS_FUN double GetSecFromStart() const {
    return GetNsecFromStart() / 1000000000;
  }
};

class NsecTimer {
 public:
  double time_ns_;

 public:
  NsecTimer() : time_ns_(0) {}

  CTP_INLINE_CROSS_FUN double GetNsec() const { return time_ns_; }
  CTP_INLINE_CROSS_FUN double GetUsec() const { return time_ns_ / 1000; }
  CTP_INLINE_CROSS_FUN double GetMsec() const { return time_ns_ / 1000000; }
  CTP_INLINE_CROSS_FUN double GetSec() const { return time_ns_ / 1000000000; }
};

template <typename T>
class TimerBase : public TimepointBase<T>, public NsecTimer {
 public:
  /** Constructor */
  CTP_INLINE_CROSS_FUN
  TimerBase() {}

  /** Resume timer */
  CTP_INLINE_CROSS_FUN void Resume() { TimepointBase<T>::Now(); }

  /** Pause timer */
  CTP_INLINE_CROSS_FUN double Pause() {
    time_ns_ += TimepointBase<T>::GetNsecFromStart();
    return time_ns_;
  }

  /** Reset timer */
  CTP_INLINE_CROSS_FUN void Reset() {
    Resume();
    time_ns_ = 0;
  }

  /** Get microseconds since timer started */
  CTP_INLINE_CROSS_FUN double GetUsFromEpoch() const {
#if CTP_IS_HOST
    std::chrono::time_point<std::chrono::system_clock> point =
        std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               point.time_since_epoch())
        .count();
#else
    return 0;
#endif
  }
};

typedef TimerBase<std::chrono::high_resolution_clock> HighResCpuTimer;
typedef TimerBase<std::chrono::steady_clock> HighResMonotonicTimer;
typedef HighResMonotonicTimer Timer;
typedef TimepointBase<std::chrono::high_resolution_clock> HighResCpuTimepoint;
typedef TimepointBase<std::chrono::steady_clock> HighResMonotonicTimepoint;
typedef HighResMonotonicTimepoint Timepoint;

/** Timer that measures actual CPU time for the calling thread */
class CpuTimer {
 public:
  double time_ns_ = 0;
#ifdef _WIN32
  // Total kernel+user time in 100ns ticks at start
  unsigned long long start_ns_ = 0;
#else
  struct timespec start_{0, 0};
#endif

#ifdef _WIN32
  static CTP_INLINE_CROSS_FUN unsigned long long ThreadCpuNs() {
    FILETIME create_t, exit_t, kernel_t, user_t;
    GetThreadTimes(GetCurrentThread(), &create_t, &exit_t, &kernel_t, &user_t);
    auto to_u64 = [](FILETIME f) {
      ULARGE_INTEGER u; u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime;
      return u.QuadPart;  // 100-ns intervals
    };
    return (to_u64(kernel_t) + to_u64(user_t)) * 100ull;
  }
#endif

  CTP_INLINE_CROSS_FUN void Resume() {
#ifdef _WIN32
    start_ns_ = ThreadCpuNs();
#else
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start_);
#endif
  }
  CTP_INLINE_CROSS_FUN double Pause() {
#ifdef _WIN32
    time_ns_ += static_cast<double>(ThreadCpuNs() - start_ns_);
#else
    struct timespec end;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
    time_ns_ += (end.tv_sec - start_.tv_sec) * 1e9
              + (end.tv_nsec - start_.tv_nsec);
#endif
    return time_ns_;
  }
  CTP_INLINE_CROSS_FUN void Reset() { time_ns_ = 0; Resume(); }
  CTP_INLINE_CROSS_FUN double GetNsec() const { return time_ns_; }
  CTP_INLINE_CROSS_FUN double GetUsec() const { return time_ns_ / 1000; }
  CTP_INLINE_CROSS_FUN double GetMsec() const { return time_ns_ / 1e6; }
  CTP_INLINE_CROSS_FUN double GetSec() const { return time_ns_ / 1e9; }
};

template <int IDX>
class PeriodicRun {
 public:
  HighResMonotonicTimer timer_;

  PeriodicRun() { timer_.Resume(); }

  template <typename LAMBDA>
  void Run(size_t max_nsec, LAMBDA &&lambda) {
    size_t nsec = timer_.GetNsecFromStart();
    if (nsec >= max_nsec) {
      lambda();
      timer_.Reset();
    }
  }
};

#define CTP_PERIODIC(IDX) \
  ctp::CrossSingleton<ctp::PeriodicRun<IDX>>::GetInstance()

}  // namespace ctp

#endif  // CTP_TIMER_H

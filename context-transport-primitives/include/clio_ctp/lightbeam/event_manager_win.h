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

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
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

#include <cstdio>
#include <string>
#include <vector>

namespace ctp::lbm {

/**
 * Windows EventManager — Linux signalfd + epoll equivalent.
 *
 * Demultiplexes two kinds of wakeups onto a single Wait() call:
 *  1. Socket I/O readiness, registered via AddEvent(fd, mask, action).
 *     Each socket is associated with a WSAEVENT via WSAEventSelect so its
 *     readiness can be observed alongside the signal event in a single
 *     WaitForMultipleObjects.
 *  2. Cross-thread / cross-process wakeup signals, registered via
 *     AddSignalEvent() and triggered by Signal(pid, tid). The recipient
 *     creates a named auto-reset Win32 event keyed by (pid, tid); the
 *     sender opens the same name and calls SetEvent. This is the Win32
 *     analogue of Linux's tgkill+signalfd+SIGUSR1 flow.
 *
 * Caveat: WaitForMultipleObjects is capped at MAXIMUM_WAIT_OBJECTS = 64
 * handles. The worker's typical fan-in (~handful of peer sockets + signal
 * event) is well under that. If a future caller registers more sockets,
 * a chained-wait pool would be required.
 */
class EventManager {
 public:
  EventManager()
      : signal_event_(nullptr),
        signal_action_(nullptr),
        next_event_id_(0) {}

  ~EventManager() {
    // Detach each socket from its event before closing the events.
    for (auto &reg : registrations_) {
      if (reg.fd_ != static_cast<int>(INVALID_SOCKET)) {
        ::WSAEventSelect(static_cast<SOCKET>(reg.fd_), nullptr, 0);
      }
      if (reg.wsa_event_ != WSA_INVALID_EVENT) {
        ::WSACloseEvent(reg.wsa_event_);
      }
    }
    if (signal_event_) {
      ::CloseHandle(signal_event_);
    }
  }

  EventManager(const EventManager &) = delete;
  EventManager &operator=(const EventManager &) = delete;

  /** Register a socket for readiness events.
   *  events is a POLLRDNORM/POLLWRNORM-style mask; it is translated into the
   *  matching FD_READ/FD_WRITE/FD_ACCEPT/FD_CLOSE WSA mask under the hood. */
  int AddEvent(int fd, uint32_t events = POLLRDNORM,
               EventAction *action = nullptr) {
    long wsa_mask = TranslatePollMaskToWsa(events);

    // If the fd is already registered, just refresh the mask + action and
    // keep the existing WSAEVENT (this mirrors the Linux EPOLL_CTL_MOD path).
    for (auto &reg : registrations_) {
      if (reg.fd_ == fd) {
        if (::WSAEventSelect(static_cast<SOCKET>(fd), reg.wsa_event_,
                              wsa_mask) == SOCKET_ERROR) {
          HLOG(kError, "EventManager::AddEvent: WSAEventSelect MOD failed: {}",
               ::WSAGetLastError());
          return -1;
        }
        reg.action_ = action;
        reg.wsa_mask_ = wsa_mask;
        return reg.event_id_;
      }
    }

    WSAEVENT ev = ::WSACreateEvent();
    if (ev == WSA_INVALID_EVENT) {
      HLOG(kError, "EventManager::AddEvent: WSACreateEvent failed: {}",
           ::WSAGetLastError());
      return -1;
    }
    if (::WSAEventSelect(static_cast<SOCKET>(fd), ev, wsa_mask) ==
        SOCKET_ERROR) {
      HLOG(kError, "EventManager::AddEvent: WSAEventSelect ADD failed: {}",
           ::WSAGetLastError());
      ::WSACloseEvent(ev);
      return -1;
    }
    int event_id = next_event_id_++;
    registrations_.push_back({fd, event_id, ev, wsa_mask, action});
    return event_id;
  }

  /** Detach an fd before the socket is closed. Equivalent to the Linux
   *  RemoveEvent(EPOLL_CTL_DEL) path. Must run *before* closesocket(). */
  void RemoveEvent(int fd) {
    for (size_t i = 0; i < registrations_.size();) {
      if (registrations_[i].fd_ == fd) {
        // Disassociate before closing the event handle.
        ::WSAEventSelect(static_cast<SOCKET>(fd), nullptr, 0);
        if (registrations_[i].wsa_event_ != WSA_INVALID_EVENT) {
          ::WSACloseEvent(registrations_[i].wsa_event_);
        }
        registrations_.erase(registrations_.begin() + i);
      } else {
        ++i;
      }
    }
  }

  /** Create the per-thread named signal event. The current thread's id is
   *  used to derive the name; Signal(pid, tid) on any thread opens the same
   *  name and calls SetEvent to wake us. */
  int AddSignalEvent(EventAction *action = nullptr) {
    std::string name = SignalEventName(
        static_cast<int>(::GetCurrentProcessId()),
        static_cast<int>(::GetCurrentThreadId()));
    signal_event_ = ::CreateEventA(nullptr,
                                    /*manual_reset=*/FALSE,
                                    /*initial_state=*/FALSE,
                                    name.c_str());
    if (!signal_event_) {
      HLOG(kError, "EventManager::AddSignalEvent: CreateEventA('{}') failed: {}",
           name, ::GetLastError());
      return -1;
    }
    signal_action_ = action;
    return next_event_id_++;
  }

  /** Cross-thread wakeup of a specific (pid, tid). Looks up the named event
   *  the target thread created in AddSignalEvent and pulses it. Returns 0
   *  on success, -1 if the target hasn't registered (the name doesn't exist
   *  yet) or any other failure. */
  static int Signal(int runtime_pid, int tid) {
    std::string name = SignalEventName(runtime_pid, tid);
    HANDLE h = ::OpenEventA(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!h) {
      // Common, non-fatal: the target hasn't called AddSignalEvent yet.
      return -1;
    }
    bool ok = ::SetEvent(h) != 0;
    ::CloseHandle(h);
    return ok ? 0 : -1;
  }

  /** Convenience: signal by raw event handle (no name lookup). Used by the
   *  in-process owner of the EventManager. */
  static int Signal(HANDLE event_handle) {
    return ::SetEvent(event_handle) ? 0 : -1;
  }

  /** Block until at least one socket becomes ready or the signal event
   *  fires. timeout_us < 0 means wait indefinitely. Returns the number of
   *  registrations whose action_ ran (signal-event firing counts as 1). */
  int Wait(int timeout_us = -1) {
    DWORD timeout_ms;
    if (timeout_us < 0) {
      timeout_ms = INFINITE;
    } else {
      timeout_ms = static_cast<DWORD>((timeout_us + 999) / 1000);
      if (timeout_ms < 1 && timeout_us > 0) timeout_ms = 1;
    }

    // Build the wait array: signal event first (so its index is fixed at 0),
    // then one entry per registered socket.
    std::vector<HANDLE> wait_handles;
    wait_handles.reserve(1 + registrations_.size());
    if (signal_event_) wait_handles.push_back(signal_event_);
    for (auto &reg : registrations_) {
      wait_handles.push_back(reg.wsa_event_);
    }
    if (wait_handles.empty()) {
      // Nothing to wait on — preserve Linux behavior of returning 0 on idle.
      if (timeout_ms != 0) ::Sleep(timeout_ms == INFINITE ? 0 : timeout_ms);
      return 0;
    }
    if (wait_handles.size() > MAXIMUM_WAIT_OBJECTS) {
      HLOG(kError,
           "EventManager::Wait: handle count {} exceeds MAXIMUM_WAIT_OBJECTS "
           "({}); chained-wait pool not implemented",
           wait_handles.size(), MAXIMUM_WAIT_OBJECTS);
      return -1;
    }

    DWORD r = ::WaitForMultipleObjectsEx(
        static_cast<DWORD>(wait_handles.size()),
        wait_handles.data(),
        /*wait_all=*/FALSE,
        timeout_ms,
        /*alertable=*/FALSE);

    if (r == WAIT_TIMEOUT) return 0;
    if (r == WAIT_FAILED) {
      HLOG(kError, "EventManager::Wait: WaitForMultipleObjectsEx failed: {}",
           ::GetLastError());
      return -1;
    }

    int fired = 0;

    // WaitForMultipleObjects returns the *lowest* signaled index; we still
    // want to drain everything else that's also ready (matches the Linux
    // epoll_wait batch semantics). Walk every handle non-blockingly.
    bool signal_ready =
        signal_event_ &&
        ::WaitForSingleObject(signal_event_, 0) == WAIT_OBJECT_0;
    if (signal_ready) {
      if (signal_action_) {
        EventInfo info;
        info.trigger_ = {-1, 0};
        info.events_ = 0;
        info.action_ = signal_action_;
        signal_action_->Run(info);
      }
      ++fired;
    }

    for (auto &reg : registrations_) {
      WSANETWORKEVENTS net_events;
      if (::WSAEnumNetworkEvents(static_cast<SOCKET>(reg.fd_),
                                  reg.wsa_event_, &net_events) ==
          SOCKET_ERROR) {
        // Not necessarily an error — the event may not be signaled.
        continue;
      }
      if (net_events.lNetworkEvents == 0) continue;
      if (reg.action_) {
        EventInfo info;
        info.trigger_ = {reg.fd_, reg.event_id_};
        info.events_ = TranslateWsaMaskToPoll(net_events.lNetworkEvents);
        info.action_ = reg.action_;
        reg.action_->Run(info);
      }
      ++fired;
    }

    return fired;
  }

  int GetEpollFd() const { return -1; }

  // Linux returns the signalfd integer; on Windows there isn't a meaningful
  // fd, so we expose the HANDLE address as an opaque non-negative integer for
  // any test that just checks ">= 0" liveness.
  int GetSignalFd() const {
    return signal_event_ ? static_cast<int>(
               reinterpret_cast<intptr_t>(signal_event_) & 0x7FFFFFFF)
                          : -1;
  }

 private:
  /** Stable name for the (pid, tid)-keyed signal event. Local\\ keeps the
   *  name in the per-session namespace so unrelated logins don't collide. */
  static std::string SignalEventName(int pid, int tid) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Local\\chi_em_%d_%d", pid, tid);
    return std::string(buf);
  }

  /** Map POLLRDNORM/POLLWRNORM/POLLERR-style mask -> WSAEventSelect mask. */
  static long TranslatePollMaskToWsa(uint32_t poll_mask) {
    long m = 0;
    if (poll_mask & (POLLRDNORM | POLLIN)) m |= FD_READ | FD_ACCEPT;
    if (poll_mask & (POLLWRNORM | POLLOUT)) m |= FD_WRITE | FD_CONNECT;
    if (poll_mask & (POLLHUP | POLLERR)) m |= FD_CLOSE;
    if (m == 0) m = FD_READ | FD_ACCEPT | FD_CLOSE;  // Sane default
    return m;
  }

  /** Map back from a WSAEventSelect fire mask to the POLL* bits the
   *  cross-platform callers expect to see in EventInfo::events_. */
  static uint32_t TranslateWsaMaskToPoll(long wsa_mask) {
    uint32_t out = 0;
    if (wsa_mask & (FD_READ | FD_ACCEPT)) out |= POLLRDNORM;
    if (wsa_mask & (FD_WRITE | FD_CONNECT)) out |= POLLWRNORM;
    if (wsa_mask & FD_CLOSE) out |= POLLHUP;
    return out;
  }

  HANDLE signal_event_;
  EventAction *signal_action_;
  int next_event_id_;

  struct EventRegistration {
    int fd_;
    int event_id_;
    WSAEVENT wsa_event_;
    long wsa_mask_;
    EventAction *action_;
  };
  std::vector<EventRegistration> registrations_;
};

}  // namespace ctp::lbm

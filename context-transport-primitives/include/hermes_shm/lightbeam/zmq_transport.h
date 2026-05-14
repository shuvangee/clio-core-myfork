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
#if HSHM_ENABLE_ZMQ
#ifndef _WIN32
#include <unistd.h>
#endif
#include <zmq.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hermes_shm/introspect/system_info.h"
#include "hermes_shm/util/logging.h"
#include "lightbeam.h"
#include "posix_socket.h"

namespace hshm::lbm {

/** No-op free callback for zmq_msg_init_data zero-copy sends */
static inline void zmq_noop_free(void *data, void *hint) {
  (void)data;
  (void)hint;
}

// ===========================================================================
// Per-phase timing instrumentation for ZeroMqTransport::Send / Recv.
//
// Diagnostic only. Enabled when the env var CHI_LBM_ZMQ_STATS=1 (default
// off so production paths stay slim). When enabled, each Send and Recv
// records nanosecond timings into process-wide atomic counters; call
// ZmqStats::DumpStats() at the end of a run to see the breakdown.
//
// Phase columns:
//   send_lock_ns       std::lock_guard<send_mtx_> acquire
//   send_meta_ser_ns   msgpack-serialize LbmMeta into vector<char>
//   send_id_frame_ns   zmq_send(identity)  (ROUTER mode only)
//   send_delim_frame_ns zmq_send("")       (ROUTER or DEALER)
//   send_meta_frame_ns zmq_send(meta_str)
//   send_bulk_frame_ns zmq_msg_send(bulk)  (summed across all bulks)
//   send_total_ns      end-to-end Send
//
//   recv_lock_ns       std::lock_guard<recv_mtx_>
//   recv_id_frame_ns   zmq_msg_recv(identity)
//   recv_delim_frame_ns zmq_msg_recv(delim)
//   recv_meta_frame_ns zmq_msg_recv(meta)
//   recv_meta_deser_ns msgpack-deserialize meta into LbmMeta
//   recv_bulk_frame_ns zmq_msg_recv(bulk)  (summed across all bulks)
//   recv_total_ns      end-to-end Recv
//
// EAGAIN paths still increment counts so we can see how much idle
// polling vs real work.
class ZmqStats {
 public:
  static bool Enabled() {
    static const bool e = []() {
      const char *v = std::getenv("CHI_LBM_ZMQ_STATS");
      return v && *v && std::atoi(v) != 0;
    }();
    return e;
  }

  // Send
  static std::atomic<uint64_t> send_count;
  static std::atomic<uint64_t> send_eagain;
  static std::atomic<uint64_t> send_bulk_count;
  static std::atomic<uint64_t> send_bulk_bytes;
  static std::atomic<uint64_t> send_lock_ns;
  static std::atomic<uint64_t> send_meta_ser_ns;
  static std::atomic<uint64_t> send_id_frame_ns;
  static std::atomic<uint64_t> send_delim_frame_ns;
  static std::atomic<uint64_t> send_meta_frame_ns;
  static std::atomic<uint64_t> send_bulk_frame_ns;
  static std::atomic<uint64_t> send_total_ns;

  // Recv
  static std::atomic<uint64_t> recv_count;
  static std::atomic<uint64_t> recv_eagain;
  static std::atomic<uint64_t> recv_bulk_count;
  static std::atomic<uint64_t> recv_bulk_bytes;
  static std::atomic<uint64_t> recv_lock_ns;
  static std::atomic<uint64_t> recv_id_frame_ns;
  static std::atomic<uint64_t> recv_delim_frame_ns;
  static std::atomic<uint64_t> recv_meta_frame_ns;
  static std::atomic<uint64_t> recv_meta_deser_ns;
  static std::atomic<uint64_t> recv_bulk_frame_ns;
  static std::atomic<uint64_t> recv_total_ns;

  // Print a per-phase summary using HLOG. Safe to call from any thread.
  static void DumpStats(const std::string &tag) {
    if (!Enabled()) return;
    auto load = [](std::atomic<uint64_t> &c) {
      return c.load(std::memory_order_relaxed);
    };
    uint64_t sc = load(send_count);
    uint64_t rc = load(recv_count);
    auto avg_us = [](uint64_t total_ns, uint64_t n) {
      return (n == 0) ? 0.0 : (static_cast<double>(total_ns) / n / 1000.0);
    };
    auto pct = [](uint64_t part, uint64_t whole) {
      return (whole == 0) ? 0.0
                          : (100.0 * static_cast<double>(part) / whole);
    };

    uint64_t s_total = load(send_total_ns);
    HLOG(kInfo,
         "[ZmqStats {} SEND] n={} eagain={} bulks={} bulk_bytes={} "
         "avg_total_us={:.2f} (lock={:.1f}% meta_ser={:.1f}% id={:.1f}% "
         "delim={:.1f}% meta={:.1f}% bulk={:.1f}%)",
         tag, sc, load(send_eagain), load(send_bulk_count),
         load(send_bulk_bytes), avg_us(s_total, sc),
         pct(load(send_lock_ns), s_total),
         pct(load(send_meta_ser_ns), s_total),
         pct(load(send_id_frame_ns), s_total),
         pct(load(send_delim_frame_ns), s_total),
         pct(load(send_meta_frame_ns), s_total),
         pct(load(send_bulk_frame_ns), s_total));

    uint64_t r_total = load(recv_total_ns);
    HLOG(kInfo,
         "[ZmqStats {} RECV] n={} eagain={} bulks={} bulk_bytes={} "
         "avg_total_us={:.2f} (lock={:.1f}% id={:.1f}% delim={:.1f}% "
         "meta={:.1f}% meta_deser={:.1f}% bulk={:.1f}%)",
         tag, rc, load(recv_eagain), load(recv_bulk_count),
         load(recv_bulk_bytes), avg_us(r_total, rc),
         pct(load(recv_lock_ns), r_total),
         pct(load(recv_id_frame_ns), r_total),
         pct(load(recv_delim_frame_ns), r_total),
         pct(load(recv_meta_frame_ns), r_total),
         pct(load(recv_meta_deser_ns), r_total),
         pct(load(recv_bulk_frame_ns), r_total));

    HLOG(kInfo,
         "[ZmqStats {} ABS_NS] s_total={} s_lock={} s_meta_ser={} "
         "s_id={} s_delim={} s_meta={} s_bulk={} | "
         "r_total={} r_lock={} r_id={} r_delim={} r_meta={} "
         "r_meta_deser={} r_bulk={}",
         tag, s_total, load(send_lock_ns), load(send_meta_ser_ns),
         load(send_id_frame_ns), load(send_delim_frame_ns),
         load(send_meta_frame_ns), load(send_bulk_frame_ns),
         r_total, load(recv_lock_ns), load(recv_id_frame_ns),
         load(recv_delim_frame_ns), load(recv_meta_frame_ns),
         load(recv_meta_deser_ns), load(recv_bulk_frame_ns));
  }

  // Lightweight nanosecond stamper. Uses steady_clock which is per-thread
  // monotonic on Linux (CLOCK_MONOTONIC) — ~30 ns/call on modern CPUs,
  // small relative to the multi-µs ZMQ frames we're measuring.
  static inline uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

inline std::atomic<uint64_t> ZmqStats::send_count{0};
inline std::atomic<uint64_t> ZmqStats::send_eagain{0};
inline std::atomic<uint64_t> ZmqStats::send_bulk_count{0};
inline std::atomic<uint64_t> ZmqStats::send_bulk_bytes{0};
inline std::atomic<uint64_t> ZmqStats::send_lock_ns{0};
inline std::atomic<uint64_t> ZmqStats::send_meta_ser_ns{0};
inline std::atomic<uint64_t> ZmqStats::send_id_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::send_delim_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::send_meta_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::send_bulk_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::send_total_ns{0};

inline std::atomic<uint64_t> ZmqStats::recv_count{0};
inline std::atomic<uint64_t> ZmqStats::recv_eagain{0};
inline std::atomic<uint64_t> ZmqStats::recv_bulk_count{0};
inline std::atomic<uint64_t> ZmqStats::recv_bulk_bytes{0};
inline std::atomic<uint64_t> ZmqStats::recv_lock_ns{0};
inline std::atomic<uint64_t> ZmqStats::recv_id_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::recv_delim_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::recv_meta_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::recv_meta_deser_ns{0};
inline std::atomic<uint64_t> ZmqStats::recv_bulk_frame_ns{0};
inline std::atomic<uint64_t> ZmqStats::recv_total_ns{0};

/** Free zmq_msg_t handles stored in Bulk::desc from zero-copy recv */
static inline void ClearZmqRecvHandles(LbmMeta<> &meta) {
  for (auto &bulk : meta.recv) {
    if (bulk.desc) {
      zmq_msg_t *msg = static_cast<zmq_msg_t*>(bulk.desc);
      zmq_msg_close(msg);
      delete msg;
      bulk.desc = nullptr;
    }
  }
}

/** Action that reads ZMQ_EVENTS when epoll fires on ZMQ_FD.
 *  Required by ZMQ docs: the FD is edge-triggered and won't
 *  re-arm until the application reads ZMQ_EVENTS. */
class ZmqFiredAction : public EventAction {
 public:
  void *socket_;
  explicit ZmqFiredAction(void *socket) : socket_(socket) {}
  void Run(const EventInfo &event) override {
    (void)event;
    int zmq_events = 0;
    ::size_t opt_len = sizeof(zmq_events);  // LCOV_EXCL_LINE
    zmq_getsockopt(socket_, ZMQ_EVENTS, &zmq_events, &opt_len);
  }
};

class ZeroMqTransport : public Transport {
 private:
  static void* GetSharedContext() {
    // CtxOwner holds the shared ZMQ context and destroys it at program exit,
    // ensuring libzmq releases its internal resources and LeakSanitizer is clean.
    struct CtxOwner {
      void* ctx = nullptr;
      std::mutex mtx;
      ~CtxOwner() {
        if (ctx) {
          // zmq_ctx_shutdown() causes all blocking ZMQ calls on open sockets
          // to return immediately with ETERM.  This unblocks any background
          // receive threads (e.g. RecvZmqClientThread) that are polling the
          // socket, allowing them to exit cleanly.  zmq_ctx_destroy() would
          // otherwise block forever if a socket is still open (because the
          // Chimaera singleton is heap-allocated and its destructor -- which
          // calls ClientFinalize / closes the socket -- is never invoked).
          zmq_ctx_shutdown(ctx);
          zmq_ctx_destroy(ctx);
          ctx = nullptr;
        }
      }
    };
    static CtxOwner owner;
    std::lock_guard<std::mutex> lock(owner.mtx);
    if (!owner.ctx) {
      owner.ctx = zmq_ctx_new();
      // I/O thread count. Default 2 saturated at 64+ nodes during SWIM
      // probe rounds + cross-node SendIn fan-out (each chimaera daemon
      // talks to N-1 peers; N²=4096 connections at N=64 is enough to
      // bottleneck 2 I/O threads). 8 scales comfortably to ~512.
      // Override at runtime via CHI_ZMQ_IO_THREADS env if needed.
      const char *iot_env = std::getenv("CHI_ZMQ_IO_THREADS");
      int iot = (iot_env && *iot_env) ? std::atoi(iot_env) : 8;
      if (iot < 1) iot = 1;
      zmq_ctx_set(owner.ctx, ZMQ_IO_THREADS, iot);
      HLOG(kInfo, "[ZeroMqTransport] Created shared context with {} I/O threads", iot);
    }
    return owner.ctx;
  }

 public:
  // Wire topology. kRouterDealer is the default (and only previously
  // supported) layout: ROUTER on the server, DEALER on each client, with
  // identity + delimiter frames around every multipart logical message.
  // kPushPull uses PUSH on the client and PULL on the server, skipping
  // the identity + delimiter frames entirely. PUSH/PULL has no
  // identity-based return routing, so the caller is responsible for
  // propagating the sender id inside the meta payload if it needs to
  // route responses back (chimaera already encodes return_node_ in
  // PoolQuery, so peer-to-peer traffic doesn't lose anything by
  // dropping identities).
  enum class Topology { kRouterDealer, kPushPull };

  explicit ZeroMqTransport(TransportMode mode, const std::string& addr,
                           const std::string& protocol = "tcp", int port = 8192,
                           Topology topology = Topology::kRouterDealer)
      : Transport(mode),
        addr_(addr),
        protocol_(protocol),
        port_(port),
        topology_(topology),
        zmq_fired_action_(nullptr) {
    type_ = TransportType::kZeroMq;
    sock::InitSocketLib();

    // Optional: pin TCP traffic to a specific local network interface on
    // multi-rail fabrics (e.g. Aurora's Slingshot HSN where each compute
    // node has hsn0..hsn7). When LIGHTBEAM_TCP_DEVICE is set:
    //   - ROUTER (server) binds the device's IP directly: tcp://hsn0:port
    //   - DEALER (client) source-binds outbound traffic to the device
    //     using ZMQ's `<source>;<destination>` endpoint syntax, so
    //     connect() routes via the chosen NIC regardless of how the
    //     destination FQDN resolves. Both ends pinned to the same device
    //     -> symmetric routing -> no asymmetric drops at scale.
    const char *bind_device_env = std::getenv("LIGHTBEAM_TCP_DEVICE");
    std::string bind_device =
        (bind_device_env && *bind_device_env) ? bind_device_env : "";

    // Loopback endpoints must stay on the host's lo interface — clients
    // connecting to 127.0.0.1 / ::1 / localhost can't reach a socket
    // pinned to a routed NIC, and the server's local-only ROUTER (used
    // by same-host wrp_cte / runtime clients) MUST keep listening on
    // 127.0.0.1 even when LIGHTBEAM_TCP_DEVICE is set for cross-node
    // traffic. Otherwise every same-host client times out in
    // WaitForLocalServer.
    auto is_loopback_addr = [](const std::string &a) {
      return a == "127.0.0.1" || a == "::1" || a == "localhost";
    };

    std::string full_url;
    if (protocol_ == "ipc") {
      full_url = "ipc://" + addr_;
    } else if (!bind_device.empty() && !is_loopback_addr(addr_) &&
               mode == TransportMode::kClient) {
      // Client: source-bind outbound traffic to the chosen NIC so
      // connect() routes via that rail regardless of how the
      // destination FQDN resolves. ZMQ syntax:
      //   tcp://<src_dev>:0;<dst_addr>:<dst_port>
      full_url = protocol_ + "://" + bind_device + ":0;" + addr_ + ":" +
                 std::to_string(port_);
    } else {
      // Server: keep the original bind address. Aurora HSN FQDNs are
      // multi-A (one entry per rail); pinning the server to a single
      // rail's IP makes peer DEALERs whose FQDN lookup landed on a
      // different rail unable to reach us. Listening on 0.0.0.0
      // (every interface) lets any rail accept connections; the
      // client-side source-bind above is what makes outbound
      // routing symmetric.
      full_url = protocol_ + "://" + addr_ + ":" + std::to_string(port_);
    }

    if (mode == TransportMode::kClient) {
      // Client socket: DEALER for ROUTER/DEALER topology, PUSH for
      // PUSH/PULL. Both connect-style; only the socket type differs.
      ctx_ = GetSharedContext();
      owns_ctx_ = false;
      int sock_type =
          (topology_ == Topology::kPushPull) ? ZMQ_PUSH : ZMQ_DEALER;
      socket_ = zmq_socket(ctx_, sock_type);

      // ZMQ_IDENTITY is only meaningful for DEALER → ROUTER routing.
      // PUSH/PULL doesn't track sender identity, so skip the setopt.
      if (topology_ != Topology::kPushPull) {
        char hostname_buf[64] = {};
        gethostname(hostname_buf, sizeof(hostname_buf) - 1);
        uint32_t pid = static_cast<uint32_t>(hshm::SystemInfo::GetPid());
        std::string identity = std::string(hostname_buf) + ":" +
                                std::to_string(pid);
        zmq_setsockopt(socket_, ZMQ_IDENTITY, identity.data(),
                        identity.size());
      }

      HLOG(kDebug, "ZeroMqTransport(DEALER) connecting to URL: {}", full_url);

      int immediate = 0;
      zmq_setsockopt(socket_, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));

      int timeout = 5000;
      zmq_setsockopt(socket_, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));

      int sndbuf = 4 * 1024 * 1024;
      zmq_setsockopt(socket_, ZMQ_SNDBUF, &sndbuf, sizeof(sndbuf));

      int rcvbuf = 4 * 1024 * 1024;
      zmq_setsockopt(socket_, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));

      // ZMQ socket-level high water marks. Default = 1000 messages.
      // With 1 MiB CTE blob payloads, that's only ~1 GB of queue
      // headroom. When a single FUSE-driven workload submits tens of
      // thousands of cross-node PutBlob tasks (24 ranks * 256 pages
      // / 2 nodes ~= 3 k cross-node tasks/node), the queue saturates
      // and zmq_send blocks (or drops with EAGAIN if DONTWAIT) for
      // long enough that the net worker can't reach Recv() and the
      // bidirectional flow deadlocks. Bump both HWMs to 100 k so the
      // ZMQ I/O thread + TCP path (4 MiB SNDBUF/RCVBUF) is the
      // bottleneck, not the application-side queue.
      int sndhwm = 100000;
      zmq_setsockopt(socket_, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
      int rcvhwm = 100000;
      zmq_setsockopt(socket_, ZMQ_RCVHWM, &rcvhwm, sizeof(rcvhwm));

      // ZMTP heartbeat: detect dead connections within seconds
      // ZMTP heartbeat: at scale (>=64 nodes) the daemon's ROUTER
      // gets busy with cross-node SWIM probes and can't respond to a
      // local DEALER's ZMTP greeting within the prior 3s window —
      // observed at iow_s64 as a HANDSHAKE_FAILED_NO_DETAIL value=32
      // (EPIPE) loop on tcp://127.0.0.1:9416. Bump to 30s so the
      // greeting can complete even under heavy probe traffic.
      int hb_ivl = 5000;     // ZMTP PING every 5 s
      zmq_setsockopt(socket_, ZMQ_HEARTBEAT_IVL, &hb_ivl, sizeof(hb_ivl));
      int hb_timeout = 30000;  // dead after 30 s of no traffic
      zmq_setsockopt(socket_, ZMQ_HEARTBEAT_TIMEOUT, &hb_timeout, sizeof(hb_timeout));
      int hb_ttl = 30000;
      zmq_setsockopt(socket_, ZMQ_HEARTBEAT_TTL, &hb_ttl, sizeof(hb_ttl));

      int rc = zmq_connect(socket_, full_url.c_str());
      if (rc == -1) {
        std::string err = "ZeroMqTransport(DEALER) failed to connect to URL '" +
                          full_url + "': " + zmq_strerror(zmq_errno());
        zmq_close(socket_);
        throw std::runtime_error(err);
      }

      zmq_pollitem_t poll_item = {socket_, 0, ZMQ_POLLOUT, 0};
      int poll_timeout_ms = 5000;
      int poll_rc = zmq_poll(&poll_item, 1, poll_timeout_ms);

      if (poll_rc < 0) {
        HLOG(kError, "[ZeroMqTransport] Poll failed for {}: {}", full_url,
             zmq_strerror(zmq_errno()));
      } else if (poll_rc == 0) {
        HLOG(kWarning,
             "[ZeroMqTransport] Poll timeout - connection to {} may not be ready",
             full_url);
      }

      const char *cli_kind =
          (topology_ == Topology::kPushPull) ? "PUSH" : "DEALER";
      HLOG(kDebug, "ZeroMqTransport({}) connected to {}", cli_kind, full_url);
      zmq_fired_action_.socket_ = socket_;
      StartMonitor(cli_kind, full_url);
    } else {
      // Server socket: ROUTER for ROUTER/DEALER topology, PULL for
      // PUSH/PULL. Both bind-style; only the socket type and
      // identity-related setopts differ.
      ctx_ = zmq_ctx_new();
      owns_ctx_ = true;
      const char *iot_env = std::getenv("CHI_ZMQ_IO_THREADS");
      int iot = (iot_env && *iot_env) ? std::atoi(iot_env) : 8;
      if (iot < 1) iot = 1;
      zmq_ctx_set(ctx_, ZMQ_IO_THREADS, iot);
      int sock_type =
          (topology_ == Topology::kPushPull) ? ZMQ_PULL : ZMQ_ROUTER;
      socket_ = zmq_socket(ctx_, sock_type);

      // ROUTER-only options: mandatory routing + handover for reconnect.
      // PULL has no identity-based dispatch so these setopts are skipped.
      if (topology_ != Topology::kPushPull) {
        int mandatory = 1;
        zmq_setsockopt(socket_, ZMQ_ROUTER_MANDATORY, &mandatory,
                        sizeof(mandatory));
        int handover = 1;
        zmq_setsockopt(socket_, ZMQ_ROUTER_HANDOVER, &handover,
                        sizeof(handover));
      }

      int rcvbuf = 4 * 1024 * 1024;
      zmq_setsockopt(socket_, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));

      int sndbuf = 4 * 1024 * 1024;
      zmq_setsockopt(socket_, ZMQ_SNDBUF, &sndbuf, sizeof(sndbuf));

      // Mirror the DEALER side: lift socket-level HWMs so the queue
      // doesn't become the bottleneck under burst cross-node CTE traffic.
      int sndhwm = 100000;
      zmq_setsockopt(socket_, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
      int rcvhwm = 100000;
      zmq_setsockopt(socket_, ZMQ_RCVHWM, &rcvhwm, sizeof(rcvhwm));

      // The ROUTER's send had no SNDTIMEO, which defaults to -1
      // (block forever). Combined with a single net worker that also
      // owns Recv(), this is what dead-locked SendOut: once the peer's
      // RX queue back-pressured, this side's zmq_send waited
      // indefinitely and Recv() never ran on the other half of the
      // flow. Cap blocking at 1 s; callers re-queue on EAGAIN.
      int sndtimeo = 1000;
      zmq_setsockopt(socket_, ZMQ_SNDTIMEO, &sndtimeo, sizeof(sndtimeo));

      // ZMTP heartbeat — same scale-friendly window as DEALER side.
      int hb_ivl = 5000;
      zmq_setsockopt(socket_, ZMQ_HEARTBEAT_IVL, &hb_ivl, sizeof(hb_ivl));
      int hb_timeout = 30000;
      zmq_setsockopt(socket_, ZMQ_HEARTBEAT_TIMEOUT, &hb_timeout, sizeof(hb_timeout));
      int hb_ttl = 30000;
      zmq_setsockopt(socket_, ZMQ_HEARTBEAT_TTL, &hb_ttl, sizeof(hb_ttl));

      const char *srv_kind =
          (topology_ == Topology::kPushPull) ? "PULL" : "ROUTER";
      HLOG(kDebug, "ZeroMqTransport({}) binding to URL: {}", srv_kind,
           full_url);
      int rc = zmq_bind(socket_, full_url.c_str());
      if (rc == -1) {
        std::string err = std::string("ZeroMqTransport(") + srv_kind +
                          ") failed to bind to URL '" + full_url + "': " +
                          zmq_strerror(zmq_errno());
        zmq_close(socket_);
        zmq_ctx_destroy(ctx_);
        throw std::runtime_error(err);
      }
      HLOG(kDebug, "ZeroMqTransport({}) bound successfully to {}", srv_kind,
           full_url);
      zmq_fired_action_.socket_ = socket_;
      StartMonitor(srv_kind, full_url);
    }
  }

  ~ZeroMqTransport() {
    HLOG(kDebug, "ZeroMqTransport destructor - closing socket to {}:{}", addr_,
         port_);

    monitor_running_.store(false);
    if (monitor_thread_.joinable()) {
      monitor_thread_.join();
    }

    int linger = 0;  // Close immediately; don't wait for unsent messages

    zmq_setsockopt(socket_, ZMQ_LINGER, &linger, sizeof(linger));

    zmq_close(socket_);
    if (owns_ctx_) {
      zmq_ctx_destroy(ctx_);
    }
    HLOG(kDebug, "ZeroMqTransport destructor - socket closed");
  }

  // ZMQ socket monitor — emits diagnostic events whenever the underlying
  // ZMTP state machine changes (CONNECT / ACCEPT / HANDSHAKE / DISCONNECT
  // etc.). Spawns a reader thread that translates each event into an
  // HLOG(kInfo) line so we can see WHY a peer-to-peer link silently fails
  // to deliver application messages.
  void StartMonitor(const std::string& kind, const std::string& url) {
    static std::atomic<uint64_t> mon_id{0};
    monitor_endpoint_ = "inproc://lbm-mon-" +
                        std::to_string(mon_id.fetch_add(1));
    int rc = zmq_socket_monitor(socket_, monitor_endpoint_.c_str(),
                                ZMQ_EVENT_ALL);
    if (rc < 0) {
      HLOG(kWarning, "[ZMQ Monitor {}] zmq_socket_monitor failed: {}",
           kind, zmq_strerror(zmq_errno()));
      return;
    }
    monitor_running_.store(true);
    monitor_thread_ = std::thread([this, kind, url]() {
      void* mon = zmq_socket(ctx_, ZMQ_PAIR);
      if (!mon) return;
      int linger = 0;
      zmq_setsockopt(mon, ZMQ_LINGER, &linger, sizeof(linger));
      int timeout = 200;
      zmq_setsockopt(mon, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
      if (zmq_connect(mon, monitor_endpoint_.c_str()) < 0) {
        HLOG(kWarning, "[ZMQ Monitor {}] connect failed: {}", kind,
             zmq_strerror(zmq_errno()));
        zmq_close(mon);
        return;
      }
      HLOG(kInfo, "[ZMQ Monitor {}] watching {}", kind, url);
      while (monitor_running_.load()) {
        zmq_msg_t evt_msg;
        zmq_msg_init(&evt_msg);
        int n = zmq_msg_recv(&evt_msg, mon, 0);
        if (n < 0) {
          if (zmq_errno() == EAGAIN) {
            zmq_msg_close(&evt_msg);
            continue;
          }
          if (zmq_errno() == ETERM) {
            zmq_msg_close(&evt_msg);
            break;
          }
          zmq_msg_close(&evt_msg);
          continue;
        }
        if (zmq_msg_size(&evt_msg) < 6) {
          zmq_msg_close(&evt_msg);
          continue;
        }
        const uint8_t* d = static_cast<const uint8_t*>(zmq_msg_data(&evt_msg));
        uint16_t event = static_cast<uint16_t>(d[0] | (d[1] << 8));
        uint32_t value = 0;
        std::memcpy(&value, d + 2, 4);
        zmq_msg_close(&evt_msg);
        zmq_msg_t addr_msg;
        zmq_msg_init(&addr_msg);
        zmq_msg_recv(&addr_msg, mon, 0);
        std::string ep(static_cast<const char*>(zmq_msg_data(&addr_msg)),
                       zmq_msg_size(&addr_msg));
        zmq_msg_close(&addr_msg);
        const char* name = "?";
        switch (event) {
          case ZMQ_EVENT_CONNECTED: name = "CONNECTED"; break;
          case ZMQ_EVENT_CONNECT_DELAYED: name = "CONNECT_DELAYED"; break;
          case ZMQ_EVENT_CONNECT_RETRIED: name = "CONNECT_RETRIED"; break;
          case ZMQ_EVENT_LISTENING: name = "LISTENING"; break;
          case ZMQ_EVENT_BIND_FAILED: name = "BIND_FAILED"; break;
          case ZMQ_EVENT_ACCEPTED: name = "ACCEPTED"; break;
          case ZMQ_EVENT_ACCEPT_FAILED: name = "ACCEPT_FAILED"; break;
          case ZMQ_EVENT_CLOSED: name = "CLOSED"; break;
          case ZMQ_EVENT_CLOSE_FAILED: name = "CLOSE_FAILED"; break;
          case ZMQ_EVENT_DISCONNECTED: name = "DISCONNECTED"; break;
          case ZMQ_EVENT_MONITOR_STOPPED: name = "MONITOR_STOPPED"; break;
          case ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
            name = "HANDSHAKE_FAILED_NO_DETAIL"; break;
          case ZMQ_EVENT_HANDSHAKE_SUCCEEDED:
            name = "HANDSHAKE_SUCCEEDED"; break;
          case ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL:
            name = "HANDSHAKE_FAILED_PROTOCOL"; break;
          case ZMQ_EVENT_HANDSHAKE_FAILED_AUTH:
            name = "HANDSHAKE_FAILED_AUTH"; break;
          default: name = "UNKNOWN"; break;
        }
        HLOG(kInfo, "[ZMQ Monitor {}] {} value={} ep={}", kind, name,
             value, ep);
        if (event == ZMQ_EVENT_MONITOR_STOPPED) break;
      }
      zmq_close(mon);
    });
  }

  Bulk Expose(const hipc::FullPtr<char>& ptr, size_t data_size,
              u32 flags) {
    Bulk bulk;
    bulk.data = ptr;
    bulk.size = data_size;
    bulk.flags = hshm::bitfield32_t(flags);
    return bulk;
  }

  // EINTR retry wrappers. ZMQ's documentation calls EINTR "interrupted
  // system call - just retry"; without retrying, any signal delivered
  // to the calling process (SIGCHLD from a child exit, SIGALRM, etc.)
  // poisons the multipart message in-flight and the DEALER socket is
  // wedged: every subsequent Send returns EINTR or a desync error
  // because the receiver is still expecting frames from the half-sent
  // message. wfbench's stress-ng subprocesses spam SIGCHLD on exit,
  // which deterministically triggers this path on every Put/Get blob.
  static inline int zmq_send_eintr(void *sock, const void *buf,
                                   size_t len, int flags) {
    while (true) {
      int rc = zmq_send(sock, buf, len, flags);
      if (rc != -1 || zmq_errno() != EINTR) return rc;
    }
  }

  static inline int zmq_msg_send_eintr(zmq_msg_t *msg, void *sock,
                                       int flags) {
    while (true) {
      int rc = zmq_msg_send(msg, sock, flags);
      if (rc != -1 || zmq_errno() != EINTR) return rc;
    }
  }

  static inline int zmq_msg_recv_eintr(zmq_msg_t *msg, void *sock,
                                       int flags) {
    while (true) {
      int rc = zmq_msg_recv(msg, sock, flags);
      if (rc != -1 || zmq_errno() != EINTR) return rc;
    }
  }

  template <typename MetaT>
  int Send(MetaT& meta, const LbmContext& ctx = LbmContext()) {
    // Optional per-phase ns timers — guarded by CHI_LBM_ZMQ_STATS env.
    // Stamps are no-ops when disabled (each is a single load of the
    // cached `Enabled()` bool plus a steady_clock::now call).
    const bool stats_enabled = ZmqStats::Enabled();
    auto stamp = [stats_enabled]() {
      return stats_enabled ? ZmqStats::NowNs() : 0;
    };
    uint64_t t_send_begin = stamp();

    // Serialize the multipart send so frames from different threads
    // don't interleave. Without this the ZMTP frame stream is racy:
    // identityA, delimiter, identityB (oops), metaA, ... — receiver
    // drops the message and our SWIM probe never gets a reply.
    uint64_t t_lock_begin = stamp();
    std::lock_guard<std::mutex> lock(send_mtx_);
    uint64_t t_lock_end = stamp();

    // Compute send_bulks before serialization so receiver knows how many
    meta.send_bulks = 0;
    for (size_t i = 0; i < meta.send.size(); ++i) {
      if (meta.send[i].flags.Any(BULK_XFER)) {
        meta.send_bulks++;
      }
    }

    uint64_t t_ser_begin = stamp();
    std::vector<char> meta_buf;
    {
      hshm::ipc::GlobalSerialize<std::vector<char>> ar(meta_buf);
      ar(meta);
      ar.Finalize();
    }
    std::string meta_str(meta_buf.begin(), meta_buf.end());
    uint64_t t_ser_end = stamp();
    size_t write_bulk_count = meta.send_bulks;

    // Send all frames non-blocking unless the caller explicitly set
    // LBM_SYNC. The HWMs above (100k msgs) make EAGAIN essentially
    // impossible under realistic CTE workloads, so we don't have to
    // worry about the partial-multipart-message orphan-frame race
    // (which would only matter if EAGAIN actually fired). DONTWAIT
    // here is the lock that prevents the net worker from blocking
    // for seconds on TCP back-pressure -- the original design
    // assumed it was non-blocking (LbmContext::flags == 0) but the
    // code path forgot to honor it and zmq_send drove the worker
    // into stalls that masquerade as cross-node delivery failures.
    int base_flags = ctx.IsSync() ? 0 : ZMQ_DONTWAIT;

    uint64_t t_id_begin = stamp(), t_id_end = 0, t_delim_end = 0;
    // Identity + delim framing is ONLY for ROUTER/DEALER. PUSH/PULL has
    // no per-message routing handshake — go straight to meta + bulk.
    const bool is_pushpull = (topology_ == Topology::kPushPull);
    if (is_pushpull) {
      t_id_end = t_id_begin;
      t_delim_end = t_id_begin;
    } else
#if !HSHM_IS_GPU
    if (IsServer() && !meta.client_info_.identity_.empty()) {
      // ROUTER: send identity + delim frames before meta.
      int rc = zmq_send_eintr(socket_, meta.client_info_.identity_.data(),
                              meta.client_info_.identity_.size(),
                              ZMQ_SNDMORE | base_flags);
      t_id_end = stamp();
      if (rc == -1) {
        HLOG(kError, "ZeroMqTransport::Send(ROUTER) - identity frame FAILED: {}",
             zmq_strerror(zmq_errno()));
        return zmq_errno();
      }
      // Send empty delimiter frame
      rc = zmq_send_eintr(socket_, "", 0, ZMQ_SNDMORE | base_flags);
      t_delim_end = stamp();
      if (rc == -1) {
        HLOG(kError, "ZeroMqTransport::Send(ROUTER) - delimiter frame FAILED: {}",
             zmq_strerror(zmq_errno()));
        return zmq_errno();
      }
    } else
#endif
    if (IsClient()) {
      t_id_end = t_id_begin;  // no identity frame for DEALER
      // DEALER mode: send empty delimiter frame
      int rc = zmq_send_eintr(socket_, "", 0, ZMQ_SNDMORE | base_flags);
      t_delim_end = stamp();
      if (rc == -1) {
        HLOG(kError, "ZeroMqTransport::Send(DEALER) - delimiter frame FAILED: {}",
             zmq_strerror(zmq_errno()));
        return zmq_errno();
      }
    } else {
      t_id_end = t_id_begin;
      t_delim_end = t_id_begin;
    }

    int flags = base_flags;
    if (write_bulk_count > 0) {
      flags |= ZMQ_SNDMORE;
    }

    uint64_t t_meta_begin = stamp();
    int rc = zmq_send_eintr(socket_, meta_str.data(), meta_str.size(), flags);
    uint64_t t_meta_end = stamp();
    if (rc == -1) {
      HLOG(kError, "ZeroMqTransport::Send - meta FAILED: {}",
           zmq_strerror(zmq_errno()));
      return zmq_errno();
    }

    uint64_t t_bulk_begin = stamp();
    size_t sent_count = 0;
    size_t bulk_bytes_sum = 0;
    for (size_t i = 0; i < meta.send.size(); ++i) {
      if (!meta.send[i].flags.Any(BULK_XFER)) {
        continue;
      }

      flags = base_flags;
      sent_count++;
      if (sent_count < write_bulk_count) {
        flags |= ZMQ_SNDMORE;
      }

      zmq_msg_t msg;
      zmq_msg_init_data(&msg, meta.send[i].data.ptr_, meta.send[i].size,
                         zmq_noop_free, nullptr);
      bulk_bytes_sum += meta.send[i].size;
      rc = zmq_msg_send_eintr(&msg, socket_, flags);
      if (rc == -1) {
        HLOG(kError, "ZeroMqTransport::Send - bulk {} FAILED: {}", i,
             zmq_strerror(zmq_errno()));
        zmq_msg_close(&msg);
        return zmq_errno();
      }
    }
    uint64_t t_bulk_end = stamp();
    uint64_t t_send_end = stamp();

    if (stats_enabled) {
      auto add = [](std::atomic<uint64_t> &c, uint64_t v) {
        c.fetch_add(v, std::memory_order_relaxed);
      };
      add(ZmqStats::send_count, 1);
      add(ZmqStats::send_bulk_count, sent_count);
      add(ZmqStats::send_bulk_bytes, bulk_bytes_sum);
      add(ZmqStats::send_lock_ns, t_lock_end - t_lock_begin);
      add(ZmqStats::send_meta_ser_ns, t_ser_end - t_ser_begin);
      add(ZmqStats::send_id_frame_ns,
          (t_id_end > t_id_begin) ? (t_id_end - t_id_begin) : 0);
      add(ZmqStats::send_delim_frame_ns,
          (t_delim_end > t_id_end) ? (t_delim_end - t_id_end) : 0);
      add(ZmqStats::send_meta_frame_ns, t_meta_end - t_meta_begin);
      add(ZmqStats::send_bulk_frame_ns, t_bulk_end - t_bulk_begin);
      add(ZmqStats::send_total_ns, t_send_end - t_send_begin);
    }
    return 0;
  }

  template <typename MetaT>
  ClientInfo Recv(MetaT& meta, const LbmContext& ctx = LbmContext()) {
    const bool stats_enabled = ZmqStats::Enabled();
    auto stamp = [stats_enabled]() {
      return stats_enabled ? ZmqStats::NowNs() : 0;
    };
    uint64_t t_recv_begin = stamp();

    // Mirror of Send's locking: a multipart Recv must consume identity /
    // delimiter / meta / bulk frames atomically or threads can take
    // each other's frames mid-message.
    uint64_t t_lock_begin = stamp();
    std::lock_guard<std::mutex> lock(recv_mtx_);
    uint64_t t_lock_end = stamp();

    ClientInfo info;
    // RecvMetadata internally records id-frame / delim-frame / meta-frame
    // / meta-deser nanoseconds via ZmqStats when enabled.
    info.rc = RecvMetadata(meta, ctx);
    if (info.rc != 0) {
      if (stats_enabled) {
        ZmqStats::recv_eagain.fetch_add(1, std::memory_order_relaxed);
        // count the abandoned lock acquisition under recv_lock_ns so we
        // don't lose it from the total.
        ZmqStats::recv_lock_ns.fetch_add(t_lock_end - t_lock_begin,
                                          std::memory_order_relaxed);
      }
      return info;
    }
#if !HSHM_IS_GPU
    // Copy identity from recv into ClientInfo
    info.identity_ = meta.client_info_.identity_;
#endif
    // Set up recv entries from send descriptors
    size_t bulk_bytes_sum = 0;
    size_t bulk_count = 0;
    for (const auto& send_bulk : meta.send) {
      Bulk recv_bulk;
      recv_bulk.size = send_bulk.size;
      recv_bulk.flags = send_bulk.flags;
      recv_bulk.data = hipc::FullPtr<char>::GetNull();
      if (send_bulk.flags.Any(BULK_XFER)) {
        ++bulk_count;
        bulk_bytes_sum += send_bulk.size;
      }
      meta.recv.push_back(recv_bulk);
    }
    uint64_t t_bulk_begin = stamp();
    info.rc = RecvBulks(meta, ctx);
    uint64_t t_bulk_end = stamp();
    uint64_t t_recv_end = stamp();

    if (stats_enabled) {
      auto add = [](std::atomic<uint64_t> &c, uint64_t v) {
        c.fetch_add(v, std::memory_order_relaxed);
      };
      add(ZmqStats::recv_count, 1);
      add(ZmqStats::recv_bulk_count, bulk_count);
      add(ZmqStats::recv_bulk_bytes, bulk_bytes_sum);
      add(ZmqStats::recv_lock_ns, t_lock_end - t_lock_begin);
      add(ZmqStats::recv_bulk_frame_ns, t_bulk_end - t_bulk_begin);
      add(ZmqStats::recv_total_ns, t_recv_end - t_recv_begin);
    }
    return info;
  }

 private:
  template <typename MetaT>
  int RecvMetadata(MetaT& meta, const LbmContext& ctx = LbmContext()) {
    (void)ctx;
    const bool stats_enabled = ZmqStats::Enabled();
    auto stamp = [stats_enabled]() {
      return stats_enabled ? ZmqStats::NowNs() : 0;
    };

    uint64_t t_id_begin = stamp();
    uint64_t t_id_end = t_id_begin;
    uint64_t t_delim_end = t_id_begin;
    const bool is_pushpull = (topology_ == Topology::kPushPull);

    // For PUSH/PULL we skip identity + delim entirely and use a single
    // DONTWAIT poll on the meta frame to mimic the existing "return
    // EAGAIN when nothing's pending" semantics.
    uint64_t t_meta_begin = stamp();
    uint64_t t_meta_end = t_meta_begin;
    zmq_msg_t msg;
    zmq_msg_init(&msg);
    int rc;
    if (is_pushpull) {
      rc = zmq_msg_recv(&msg, socket_, ZMQ_DONTWAIT);
      t_meta_end = stamp();
      if (rc == -1) {
        int err = zmq_errno();
        zmq_msg_close(&msg);
        return err;
      }
    } else if (IsServer()) {
      // ROUTER: identity frame first
      zmq_msg_t identity_msg;
      zmq_msg_init(&identity_msg);
      int rc_id = zmq_msg_recv(&identity_msg, socket_, ZMQ_DONTWAIT);
      t_id_end = stamp();
      if (rc_id == -1) {
        int err = zmq_errno();
        zmq_msg_close(&identity_msg);
        zmq_msg_close(&msg);
        return err;
      }
#if !HSHM_IS_GPU
      meta.client_info_.identity_ = std::string(
          static_cast<char*>(zmq_msg_data(&identity_msg)),
          zmq_msg_size(&identity_msg));
#endif
      zmq_msg_close(&identity_msg);

      // Delim (blocking + EINTR-retry)
      zmq_msg_t delim_msg;
      zmq_msg_init(&delim_msg);
      int rc_d = zmq_msg_recv_eintr(&delim_msg, socket_, 0);
      t_delim_end = stamp();
      zmq_msg_close(&delim_msg);
      if (rc_d == -1) {
        zmq_msg_close(&msg);
        return zmq_errno();
      }

      // Meta (blocking)
      rc = zmq_msg_recv_eintr(&msg, socket_, 0);
      t_meta_end = stamp();
    } else {
      // DEALER: just delim then meta. Delim DONTWAIT for EAGAIN check.
      zmq_msg_t delim_msg;
      zmq_msg_init(&delim_msg);
      int rc_d = zmq_msg_recv(&delim_msg, socket_, ZMQ_DONTWAIT);
      t_delim_end = stamp();
      if (rc_d == -1) {
        int err = zmq_errno();
        zmq_msg_close(&delim_msg);
        zmq_msg_close(&msg);
        return err;
      }
      zmq_msg_close(&delim_msg);
      rc = zmq_msg_recv_eintr(&msg, socket_, 0);
      t_meta_end = stamp();
    }

    if (rc == -1) {
      int err = zmq_errno();
      zmq_msg_close(&msg);
      return err;
    }

    uint64_t t_deser_begin = stamp();
    size_t msg_size = zmq_msg_size(&msg);
    try {
      std::vector<char> meta_buf(static_cast<char*>(zmq_msg_data(&msg)),
                                  static_cast<char*>(zmq_msg_data(&msg)) + msg_size);
      hshm::ipc::GlobalDeserialize<std::vector<char>> ar(meta_buf);
      ar(meta);
    } catch (const std::exception& e) {
      HLOG(kFatal,
           "ZeroMQ RecvMetadata: Deserialization failed - {} (msg_size={})",
           e.what(), msg_size);
      zmq_msg_close(&msg);
      return -1;
    }
    zmq_msg_close(&msg);
    uint64_t t_deser_end = stamp();

    if (stats_enabled) {
      auto add = [](std::atomic<uint64_t> &c, uint64_t v) {
        c.fetch_add(v, std::memory_order_relaxed);
      };
      // For DEALER, t_id_end == t_id_begin (no identity frame).
      add(ZmqStats::recv_id_frame_ns,
          (t_id_end > t_id_begin) ? (t_id_end - t_id_begin) : 0);
      add(ZmqStats::recv_delim_frame_ns,
          (t_delim_end > t_id_end) ? (t_delim_end - t_id_end) : 0);
      add(ZmqStats::recv_meta_frame_ns, t_meta_end - t_meta_begin);
      add(ZmqStats::recv_meta_deser_ns, t_deser_end - t_deser_begin);
    }
    return 0;
  }

  template <typename MetaT>
  int RecvBulks(MetaT& meta, const LbmContext& ctx = LbmContext()) {
    (void)ctx;
    size_t recv_count = 0;
    for (size_t i = 0; i < meta.recv.size(); ++i) {
      if (!meta.recv[i].flags.Any(BULK_XFER)) {
        continue;
      }
      recv_count++;
      int flags = (recv_count < meta.send_bulks) ? ZMQ_RCVMORE : 0;

      if (meta.recv[i].data.ptr_) {
        zmq_msg_t zmq_msg;
        zmq_msg_init(&zmq_msg);
        int rc = zmq_msg_recv_eintr(&zmq_msg, socket_, flags);
        if (rc == -1) {
          int err = zmq_errno();
          zmq_msg_close(&zmq_msg);
          return err;
        }
        memcpy(meta.recv[i].data.ptr_,
               zmq_msg_data(&zmq_msg), meta.recv[i].size);
        zmq_msg_close(&zmq_msg);
      } else {
        zmq_msg_t *zmq_msg = new zmq_msg_t;
        zmq_msg_init(zmq_msg);
        int rc = zmq_msg_recv_eintr(zmq_msg, socket_, flags);
        if (rc == -1) {
          int err = zmq_errno();
          zmq_msg_close(zmq_msg);
          delete zmq_msg;
          return err;
        }
        char *zmq_data = static_cast<char*>(zmq_msg_data(zmq_msg));
        meta.recv[i].data.ptr_ = zmq_data;
        meta.recv[i].data.shm_.alloc_id_ = hipc::AllocatorId::GetNull();
        meta.recv[i].data.shm_.off_ = reinterpret_cast<size_t>(zmq_data);
        meta.recv[i].desc = zmq_msg;
      }
    }
    return 0;
  }

 public:
  void ClearRecvHandles(LbmMeta<>& meta) {
    ClearZmqRecvHandles(meta);
  }

  void RegisterEventManager(EventManager &em) {
    int fd;
    size_t fd_size = sizeof(fd);
    zmq_getsockopt(socket_, ZMQ_FD, &fd, reinterpret_cast<::size_t *>(&fd_size));
    if (fd >= 0) {
      em.AddEvent(fd, kDefaultReadEvent, nullptr);
    }
  }

  std::string GetAddress() const { return addr_; }

  /** Check if the server is still alive via a TCP connect probe. */
  bool IsServerAlive(const LbmContext& ctx = LbmContext()) const {
    (void)ctx;
    if (protocol_ == "ipc") {
      // Unix domain socket — try connect
      sock::socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd == sock::kInvalidSocket) return false;
      struct sockaddr_un sun;
      std::memset(&sun, 0, sizeof(sun));
      sun.sun_family = AF_UNIX;
      std::strncpy(sun.sun_path, addr_.c_str(), sizeof(sun.sun_path) - 1);
      int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&sun),
                         sizeof(sun));
      sock::Close(fd);
      return rc == 0;
    }
    // TCP — probe addr_:port_
    sock::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == sock::kInvalidSocket) return false;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port_));
    ::inet_pton(AF_INET, addr_.c_str(), &sa.sin_addr);
    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&sa),
                       sizeof(sa));
    sock::Close(fd);
    return rc == 0;
  }

 private:
  std::string addr_;
  std::string protocol_;
  int port_;
  Topology topology_ = Topology::kRouterDealer;
  void* ctx_;
  bool owns_ctx_;
  void* socket_;
  ZmqFiredAction zmq_fired_action_;
  // ZMQ sockets are not thread-safe (per the ZeroMQ guide). Multipart
  // sends (identity / delimiter / meta / N bulk frames) issued from
  // multiple chimaera worker threads on the same socket can interleave,
  // corrupting the ZMTP frame stream. Receivers then silently drop
  // unparseable messages — appearing as e.g. SWIM HeartbeatProbe
  // timeouts on multi-node deployments. Hold these around Send/Recv so
  // each multipart message is atomic.
  std::mutex send_mtx_;
  std::mutex recv_mtx_;
  // ZMQ socket monitor — diagnostic for ZMTP-layer connection events.
  std::thread monitor_thread_;
  std::atomic<bool> monitor_running_{false};
  std::string monitor_endpoint_;
};

}  // namespace hshm::lbm

#endif  // HSHM_ENABLE_ZMQ

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

/**
 * Redis comparison benchmark — apples-to-apples mirror of wrp_cte_bench.
 *
 * Same CLI and metrics (bench_common.h); the only difference is the
 * backend: each worker holds its own hiredis connection and issues
 * SET (Put) / GET (Get) with `--depth` pipelining, exactly mirroring
 * CTE PutBlob/GetBlob. --max-total-blobs / --time-limit behave
 * identically (global keyspace split evenly across threads).
 *
 *   Put    -> SET   key value
 *   Get    -> GET   key            (keyspace populated first, untimed)
 *   PutGet -> SET then GET per key
 *
 * Connection (env, defaults): REDIS_HOST=127.0.0.1  REDIS_PORT=6379
 *
 * Built only when WRP_CORE_ENABLE_REDIS=ON (needs hiredis).
 */

#include "bench_common.h"

#include <hermes_shm/util/logging.h>
#include <hiredis/hiredis.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;
using wrp_bench::BenchArgs;

namespace {

inline bool TimeUp(const steady_clock::time_point &start, double limit_s) {
  if (limit_s <= 0.0) return false;
  return duration<double>(steady_clock::now() - start).count() >= limit_s;
}

inline long KeyIndex(long n, wrp_bench::u64 keyspace) {
  return keyspace > 0 ? static_cast<long>(n % static_cast<long>(keyspace))
                      : n;
}

struct RedisConn {
  redisContext *c = nullptr;
  explicit RedisConn(const std::string &host, int port) {
    c = redisConnect(host.c_str(), port);
  }
  ~RedisConn() { if (c) redisFree(c); }
  bool ok() const { return c && !c->err; }
};

}  // namespace

class RedisBenchmark {
 public:
  RedisBenchmark(const BenchArgs &a, std::string host, int port)
      : a_(a),
        host_(std::move(host)),
        port_(port),
        per_thread_blobs_(a.PerThreadBlobs()) {}

  bool Run() {
    PrintInfo();
    if (a_.test_case == "Put") return RunGeneric(Mode::kPut);
    if (a_.test_case == "Get") return RunGeneric(Mode::kGet);
    if (a_.test_case == "PutGet") return RunGeneric(Mode::kPutGet);
    HLOG(kError, "Unknown test case: {} (Put|Get|PutGet)", a_.test_case);
    return false;
  }

 private:
  enum class Mode { kPut, kGet, kPutGet };

  void PrintInfo() {
    HLOG(kInfo, "=== Redis Benchmark ({}:{}) ===", host_, port_);
    HLOG(kInfo, "Test: {}  Threads: {}  Depth: {}", a_.test_case, a_.threads,
         a_.depth);
    HLOG(kInfo, "I/O size: {}  io-count/thread: {}  max-total-blobs: {} "
                "({}/thread)  time-limit: {}s",
         wrp_bench::FormatSize(a_.io_size), a_.io_count, a_.max_total_blobs,
         per_thread_blobs_, a_.time_limit_s);
    HLOG(kInfo, "===========================");
  }

  long KeyspaceSize() const {
    if (per_thread_blobs_ > 0) return static_cast<long>(per_thread_blobs_);
    return a_.io_count > 0 ? a_.io_count : 1;
  }

  // Pipeline `n` SET or GET commands then drain replies. Returns false on
  // any connection/reply error.
  static bool Pipeline(redisContext *c, bool is_set,
                        const std::vector<std::string> &keys,
                        const char *val, size_t vlen) {
    for (const auto &k : keys) {
      if (is_set) {
        redisAppendCommand(c, "SET %s %b", k.c_str(), val, vlen);
      } else {
        redisAppendCommand(c, "GET %s", k.c_str());
      }
    }
    bool ok = true;
    for (size_t i = 0; i < keys.size(); ++i) {
      redisReply *r = nullptr;
      if (redisGetReply(c, reinterpret_cast<void **>(&r)) != REDIS_OK ||
          r == nullptr) {
        ok = false;
        if (r) freeReplyObject(r);
        break;
      }
      if (r->type == REDIS_REPLY_ERROR) ok = false;
      freeReplyObject(r);
    }
    return ok;
  }

  void Worker(Mode mode, size_t tid, std::atomic<bool> &err,
              std::vector<long long> &times, std::vector<wrp_bench::u64> &ops) {
    RedisConn conn(host_, port_);
    if (!conn.ok()) {
      HLOG(kError, "[t{}] redis connect failed: {}", tid,
           conn.c ? conn.c->errstr : "alloc");
      err.store(true, std::memory_order_relaxed);
      return;
    }
    redisContext *c = conn.c;
    std::vector<char> val(a_.io_size, static_cast<char>(tid & 0xFF));
    auto key = [&](long k) {
      return "blob_t" + std::to_string(tid) + "_" + std::to_string(k);
    };

    if (mode == Mode::kGet) {  // populate keyspace (untimed)
      for (long k = 0; k < KeyspaceSize(); ++k) {
        redisReply *r = static_cast<redisReply *>(redisCommand(
            c, "SET %s %b", key(k).c_str(), val.data(), val.size()));
        if (!r || r->type == REDIS_REPLY_ERROR) {
          err.store(true, std::memory_order_relaxed);
          if (r) freeReplyObject(r);
          return;
        }
        freeReplyObject(r);
      }
    }

    const bool timed = a_.time_limit_s > 0.0;
    const long target = timed ? std::numeric_limits<long>::max() : a_.io_count;
    wrp_bench::u64 done = 0;
    auto start = steady_clock::now();

    for (long i = 0; i < target; i += a_.depth) {
      if (err.load(std::memory_order_relaxed)) break;
      if (timed && TimeUp(start, a_.time_limit_s)) break;
      long batch = timed ? a_.depth : std::min<long>(a_.depth, target - i);
      std::vector<std::string> keys;
      keys.reserve(batch);
      for (long j = 0; j < batch; ++j) {
        keys.push_back(key(KeyIndex(i + j, per_thread_blobs_)));
      }
      if (mode == Mode::kPut || mode == Mode::kPutGet) {
        if (!Pipeline(c, true, keys, val.data(), val.size())) {
          HLOG(kError, "[t{}] redis SET error", tid);
          err.store(true, std::memory_order_relaxed);
        }
      }
      if (mode == Mode::kGet || mode == Mode::kPutGet) {
        if (!Pipeline(c, false, keys, val.data(), val.size())) {
          HLOG(kError, "[t{}] redis GET error", tid);
          err.store(true, std::memory_order_relaxed);
        }
      }
      done += static_cast<wrp_bench::u64>(batch);
    }

    times[tid] =
        duration_cast<microseconds>(steady_clock::now() - start).count();
    ops[tid] = done;
  }

  bool RunGeneric(Mode mode) {
    if (mode == Mode::kGet) {
      HLOG(kInfo, "Populating {} keys/thread for Get...", KeyspaceSize());
    }
    std::vector<std::thread> threads;
    std::vector<long long> times(a_.threads);
    std::vector<wrp_bench::u64> ops(a_.threads);
    std::atomic<bool> err{false};
    for (size_t i = 0; i < a_.threads; ++i) {
      threads.emplace_back(&RedisBenchmark::Worker, this, mode, i,
                           std::ref(err), std::ref(times), std::ref(ops));
    }
    for (auto &t : threads) t.join();
    wrp_bench::PrintResults(a_.test_case, a_, times, ops);
    return !err.load();
  }

  BenchArgs a_;
  std::string host_;
  int port_;
  wrp_bench::u64 per_thread_blobs_;  // a_.max_total_blobs / threads
};

int main(int argc, char **argv) {
  BenchArgs args = wrp_bench::ParseBenchArgs(argc, argv);
  if (!args.ok) return 1;

  const char *h = std::getenv("REDIS_HOST");
  const char *p = std::getenv("REDIS_PORT");
  std::string host = (h && h[0]) ? h : "127.0.0.1";
  int port = (p && p[0]) ? std::atoi(p) : 6379;

  RedisBenchmark bench(args, host, port);
  if (!bench.Run()) {
    HLOG(kError, "Redis benchmark failed (connection or command error)");
    return 1;
  }
  return 0;
}

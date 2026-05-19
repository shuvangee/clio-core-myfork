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
 * Shared throughput-benchmark scaffolding.
 *
 * Used by both wrp_cte_bench (CTE PutBlob/GetBlob) and wrp_redis_bench
 * (hiredis SET/GET) so the two are true apples-to-apples mirrors: same
 * CLI, same workload knobs, same results format. Backends only differ
 * in how a single Put/Get is issued.
 *
 * CLI (semantic flags preferred; legacy positional form still works):
 *   <bench> --op Put --threads 4 --depth 4 --io-size 1m --io-count 1000
 *   <bench> --op PutGet --threads 8 --io-size 4k --time-limit 30
 *   <bench> Put 4 4 1m 1000                # legacy positional fallback
 *
 *   --op|--test-case   Put | Get | PutGet                (default Put)
 *   --threads          worker threads                    (default 1)
 *   --depth            async requests in flight / thread  (default 1)
 *   --io-size          bytes per op (k/m/g suffixes)      (default 1m)
 *   --io-count         ops per thread                     (default 1000)
 *   --max-total-blobs  total distinct keys across ALL      (default 0 = unbounded)
 *                      threads (each thread cycles
 *                      max-total-blobs / threads keys)
 *   --time-limit       run N seconds instead of io-count  (default 0 = off)
 *   --help
 */

#ifndef WRP_BENCH_COMMON_H_
#define WRP_BENCH_COMMON_H_

#include <hermes_shm/util/logging.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace wrp_bench {

using u64 = std::uint64_t;

/** Parse a size string with optional k/K, m/M, g/G suffix (decimals ok). */
inline u64 ParseSize(const std::string &size_str) {
  std::string num_str;
  char suffix = 0;
  for (char c : size_str) {
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      num_str += c;
    } else if (c == 'k' || c == 'K' || c == 'm' || c == 'M' || c == 'g' ||
               c == 'G') {
      suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      break;
    }
  }
  if (num_str.empty()) {
    HLOG(kError, "Invalid size format: {}", size_str);
    return 0;
  }
  double size = std::stod(num_str);
  u64 mult = 1;
  switch (suffix) {
    case 'k': mult = 1024ULL; break;
    case 'm': mult = 1024ULL * 1024; break;
    case 'g': mult = 1024ULL * 1024 * 1024; break;
    default:  mult = 1; break;
  }
  return static_cast<u64>(size * static_cast<double>(mult));
}

/** Bytes -> human string. */
inline std::string FormatSize(u64 bytes) {
  if (bytes >= 1024ULL * 1024 * 1024) {
    return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
  } else if (bytes >= 1024ULL * 1024) {
    return std::to_string(bytes / (1024ULL * 1024)) + " MB";
  } else if (bytes >= 1024) {
    return std::to_string(bytes / 1024) + " KB";
  }
  return std::to_string(bytes) + " B";
}

/** MB/s from bytes over a microsecond interval. */
inline double CalcBandwidth(u64 total_bytes, double microseconds) {
  if (microseconds <= 0.0) return 0.0;
  double seconds = microseconds / 1000000.0;
  double megabytes = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
  return megabytes / seconds;
}

/** Parsed benchmark parameters (shared by every backend). */
struct BenchArgs {
  std::string test_case = "Put";  // Put | Get | PutGet
  size_t threads = 1;
  int depth = 1;
  u64 io_size = 1024ULL * 1024;   // 1 MiB
  int io_count = 1000;            // ops/thread (ignored if time_limit_s > 0)
  u64 max_total_blobs = 0;        // 0 = unbounded; else TOTAL distinct keys
                                  // across all threads (split evenly)
  double time_limit_s = 0.0;      // 0 = run io_count ops; else run for N s
  bool ok = false;                // false => caller should print usage & exit

  // Distinct keys a single thread cycles through. The global keyspace
  // (max_total_blobs) is split evenly across threads so the working set
  // is threads-independent: total unique bytes == max_total_blobs*io_size
  // regardless of --threads. 0 => unbounded (one fresh key per op).
  u64 PerThreadBlobs() const {
    if (max_total_blobs == 0) return 0;
    u64 t = threads ? threads : 1;
    return max_total_blobs / t > 0 ? max_total_blobs / t : 1;
  }
};

inline void PrintUsage(const char *argv0) {
  HLOG(kError, "Usage: {} [--op Put|Get|PutGet] [--threads N] [--depth N]",
       argv0);
  HLOG(kError, "       [--io-size 1m] [--io-count N] [--max-total-blobs N] "
               "[--time-limit SECONDS]");
  HLOG(kError, "  Legacy positional form (still supported):");
  HLOG(kError, "       {} <test_case> <threads> <depth> <io_size> <io_count>",
       argv0);
}

/**
 * Parse argv. Semantic --flags are preferred; if the first arg does not
 * start with '-' the legacy positional form
 *   <test_case> <threads> <depth> <io_size> <io_count>
 * is parsed instead (back-compat for existing CI/docker callers).
 */
inline BenchArgs ParseBenchArgs(int argc, char **argv) {
  BenchArgs a;
  auto need = [&](int i) -> const char * {
    if (i >= argc) {
      HLOG(kError, "Missing value for option {}", argv[i - 1]);
      return nullptr;
    }
    return argv[i];
  };

  if (argc >= 2 && argv[1][0] != '-') {
    // Legacy positional: <test_case> <threads> <depth> <io_size> <io_count>
    if (argc != 6) {
      PrintUsage(argv[0]);
      return a;
    }
    a.test_case = argv[1];
    a.threads = std::stoull(argv[2]);
    a.depth = std::atoi(argv[3]);
    a.io_size = ParseSize(argv[4]);
    a.io_count = std::atoi(argv[5]);
  } else {
    for (int i = 1; i < argc; ++i) {
      std::string f = argv[i];
      if (f == "--help" || f == "-h") {
        PrintUsage(argv[0]);
        return a;
      } else if (f == "--op" || f == "--test-case") {
        const char *v = need(++i); if (!v) return a; a.test_case = v;
      } else if (f == "--threads" || f == "--thread") {
        const char *v = need(++i); if (!v) return a;
        a.threads = std::stoull(v);
      } else if (f == "--depth") {
        const char *v = need(++i); if (!v) return a; a.depth = std::atoi(v);
      } else if (f == "--io-size") {
        const char *v = need(++i); if (!v) return a; a.io_size = ParseSize(v);
      } else if (f == "--io-count") {
        const char *v = need(++i); if (!v) return a; a.io_count = std::atoi(v);
      } else if (f == "--max-total-blobs") {
        const char *v = need(++i); if (!v) return a;
        a.max_total_blobs = std::stoull(v);
      } else if (f == "--time-limit") {
        const char *v = need(++i); if (!v) return a;
        a.time_limit_s = std::stod(v);
      } else {
        HLOG(kError, "Unknown option: {}", f);
        PrintUsage(argv[0]);
        return a;
      }
    }
  }

  if (a.threads == 0 || a.depth <= 0 || a.io_size == 0 ||
      (a.time_limit_s <= 0.0 && a.io_count <= 0)) {
    HLOG(kError, "Invalid parameters (threads/depth/io-size must be > 0; "
                 "need io-count > 0 or time-limit > 0)");
    PrintUsage(argv[0]);
    return a;
  }
  a.ok = true;
  return a;
}

/**
 * Print per-thread + aggregate timing/bandwidth. ops[i] is the number of
 * ops thread i actually completed (matters for --time-limit, where it is
 * not io_count); times[i] is its elapsed microseconds.
 */
inline void PrintResults(const std::string &label, const BenchArgs &a,
                         const std::vector<long long> &times,
                         const std::vector<u64> &ops) {
  long long min_t = *std::min_element(times.begin(), times.end());
  long long max_t = *std::max_element(times.begin(), times.end());
  long long sum_t = 0;
  u64 total_ops = 0;
  for (size_t i = 0; i < times.size(); ++i) {
    sum_t += times[i];
    total_ops += ops[i];
  }
  double avg_t = static_cast<double>(sum_t) / static_cast<double>(a.threads);
  u64 avg_ops = total_ops / a.threads;

  u64 per_thread_bytes = avg_ops * a.io_size;
  u64 aggregate_bytes = total_ops * a.io_size;

  HLOG(kInfo, "");
  HLOG(kInfo, "=== {} Benchmark Results ===", label);
  HLOG(kInfo, "Ops (total / avg per thread): {} / {}", total_ops, avg_ops);
  HLOG(kInfo, "Time (min/max/avg): {} / {} / {} ms", min_t / 1000.0,
       max_t / 1000.0, avg_t / 1000.0);
  HLOG(kInfo, "Bandwidth per thread (avg): {} MB/s",
       CalcBandwidth(per_thread_bytes, avg_t));
  HLOG(kInfo, "Aggregate bandwidth: {} MB/s",
       CalcBandwidth(aggregate_bytes, avg_t));
  HLOG(kInfo, "===========================");
}

}  // namespace wrp_bench

#endif  // WRP_BENCH_COMMON_H_

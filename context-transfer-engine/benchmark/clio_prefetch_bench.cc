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
 * CTE Prefetch Benchmark (semester research task, week of 2026-09-28).
 *
 * Synthetic benchmark requested by Luke: read a configurable amount of data
 * in fixed-size blocks, then spin-wait a configurable time to simulate
 * compute between reads. While spin-waiting, a client-side prefetcher issues
 * async ReorganizeBlob calls to promote upcoming blocks from the slow (disk)
 * tier to the fast (DRAM) tier, so the next read is served hot.
 *
 * Deliberately does NOT touch CTE core: prefetching here is a plain
 * client-side loop calling Client::AsyncReorganizeBlob, matching the
 * "thread-based prefetcher in the client is also fine" guidance. Blobs are
 * demoted back to the slow tier after being consumed so the fast tier's
 * footprint stays bounded to the prefetch window instead of growing with the
 * dataset. The internal DataOrganizer ("frecency", issue #738) is
 * deliberately left OFF (organizer: "none") so it cannot also be rescoring
 * blobs in the background and confounding the measurement.
 *
 * Runs BOTH baseline (no prefetch) and prefetch-enabled configurations,
 * back-to-back, multiple trials each, in one process so the comparison is
 * apples-to-apples (same machine, same session, same dataset layout).
 *
 * Usage:
 *   clio_prefetch_bench <total_mb> <block_mb> <spin_wait_ms>
 *                        <prefetch_distance> <trials>
 *
 * Example (targets Luke's ~50/50 I/O:compute best case; tune spin_wait_ms to
 * roughly match this machine's measured disk-tier block read latency):
 *   clio_prefetch_bench 256 4 40 2 5
 */

#include <clio_runtime/clio_runtime.h>
#include <clio_cte/core/core_client.h>
#include <clio_ctp/util/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {

constexpr float kFastTierScore = 1.0f;  // DRAM
constexpr float kSlowTierScore = 0.1f;  // disk

std::string TestDataDir() {
  const char *d = std::getenv("CLIO_PREFETCH_BENCH_DIR");
  return (d && *d) ? d : "/tmp/clio_prefetch_bench";
}

/** Busy-wait for `ms` milliseconds — simulates compute overlapping I/O. */
void BusyWaitMs(double ms) {
  if (ms <= 0.0) return;
  auto start = high_resolution_clock::now();
  auto target = duration<double, std::milli>(ms);
  volatile long x = 0;
  while (high_resolution_clock::now() - start < target) {
    for (int i = 0; i < 1000; ++i) x += i;
  }
  (void)x;
}

double Mean(const std::vector<double> &v) {
  if (v.empty()) return 0.0;
  return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

double StdDev(const std::vector<double> &v, double mean) {
  if (v.size() < 2) return 0.0;
  double acc = 0.0;
  for (double x : v) acc += (x - mean) * (x - mean);
  return std::sqrt(acc / static_cast<double>(v.size() - 1));
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
  return v[idx];
}

std::string BlobName(int i) { return "block_" + std::to_string(i); }

}  // namespace

struct TrialResult {
  double total_ms = 0.0;
  std::vector<double> read_latency_ms;
};

class PrefetchBenchmark {
 public:
  PrefetchBenchmark(clio::run::u64 total_bytes, clio::run::u64 block_bytes,
                    double spin_wait_ms, int prefetch_distance)
      : total_bytes_(total_bytes),
        block_bytes_(block_bytes),
        spin_wait_ms_(spin_wait_ms),
        prefetch_distance_(prefetch_distance) {
    num_blocks_ = static_cast<int>(
        (total_bytes_ + block_bytes_ - 1) / block_bytes_);
  }

  /** (Re-)create the tag and put every block on the slow tier. Not timed. */
  void ResetDataset(const std::string &tag_name) {
    auto *cte_client = CLIO_CTE_CLIENT;
    clio::cte::core::Tag tag(tag_name);

    auto buf = CLIO_IPC->AllocateBuffer(block_bytes_);
    std::vector<clio::run::Future<clio::cte::core::PutBlobTask>> puts;
    puts.reserve(num_blocks_);
    for (int i = 0; i < num_blocks_; ++i) {
      std::memset(buf.ptr_, PatternFor(i), block_bytes_);
      puts.push_back(tag.AsyncPutBlob(BlobName(i),
                                      buf.shm_.template Cast<void>(),
                                      block_bytes_, 0, kSlowTierScore));
      // Wait immediately: buf is reused for every block, so the write must
      // land before we overwrite it for the next one.
      puts.back().Wait();
    }
    CLIO_IPC->FreeBuffer(buf);
  }

  TrialResult RunTrial(const std::string &tag_name, bool prefetch_enabled) {
    auto *cte_client = CLIO_CTE_CLIENT;
    clio::cte::core::Tag tag(tag_name);
    clio::cte::core::TagId tag_id = tag.GetTagId();

    TrialResult result;
    result.read_latency_ms.reserve(num_blocks_);

    auto read_buf = CLIO_IPC->AllocateBuffer(block_bytes_);
    auto trial_start = high_resolution_clock::now();

    for (int i = 0; i < num_blocks_; ++i) {
      auto t0 = high_resolution_clock::now();
      auto get_task = cte_client->AsyncGetBlob(
          tag_id, BlobName(i), 0, block_bytes_, 0,
          read_buf.shm_.template Cast<void>());
      get_task.Wait();
      auto t1 = high_resolution_clock::now();
      if (get_task->GetReturnCode() != 0) {
        std::cerr << "  WARNING: read of " << BlobName(i)
                  << " failed (rc=" << get_task->GetReturnCode() << ")\n";
      } else if (!VerifyPattern(read_buf.ptr_, i)) {
        std::cerr << "  WARNING: data mismatch on " << BlobName(i) << "\n";
      }
      result.read_latency_ms.push_back(
          duration_cast<duration<double, std::milli>>(t1 - t0).count());

      std::vector<clio::run::Future<clio::cte::core::ReorganizeBlobTask>>
          reorg_tasks;
      if (prefetch_enabled) {
        // Promote the next `prefetch_distance_` blocks to the fast tier —
        // fire-and-forget, they complete on the runtime's own worker while
        // we busy-wait below (that overlap is the entire point).
        for (int d = 1; d <= prefetch_distance_; ++d) {
          int j = i + d;
          if (j >= num_blocks_) break;
          reorg_tasks.push_back(
              cte_client->AsyncReorganizeBlob(tag_id, BlobName(j),
                                              kFastTierScore));
        }
        // Demote the block we just consumed back to the slow tier so the
        // fast tier's footprint stays bounded to the prefetch window
        // instead of growing with the whole dataset.
        cte_client->AsyncReorganizeBlob(tag_id, BlobName(i), kSlowTierScore);
      }

      BusyWaitMs(spin_wait_ms_);

      // Do not block the loop on reorg completion in general (that would
      // erase the overlap we're trying to measure) — but on the LAST block
      // there is no further busy-wait to hide behind, so nothing to wait
      // for either; reorg tasks for blocks we haven't reached yet are left
      // to finish on their own, which is fine since a not-yet-promoted
      // block simply falls back to a normal (slow) read next iteration.
      (void)reorg_tasks;
    }

    auto trial_end = high_resolution_clock::now();
    result.total_ms =
        duration_cast<duration<double, std::milli>>(trial_end - trial_start)
            .count();

    CLIO_IPC->FreeBuffer(read_buf);
    return result;
  }

  void CleanupDataset(const std::string &tag_name) {
    auto *cte_client = CLIO_CTE_CLIENT;
    clio::cte::core::Tag tag(tag_name);
    clio::cte::core::TagId tag_id = tag.GetTagId();
    for (int i = 0; i < num_blocks_; ++i) {
      cte_client->AsyncDelBlob(tag_id, BlobName(i)).Wait();
    }
    cte_client->AsyncDelTag(tag_name).Wait();
  }

  int num_blocks() const { return num_blocks_; }

 private:
  static char PatternFor(int i) { return static_cast<char>(i & 0xFF); }
  bool VerifyPattern(const char *data, int i) const {
    char expect = PatternFor(i);
    for (clio::run::u64 k = 0; k < block_bytes_; ++k) {
      if (data[k] != expect) return false;
    }
    return true;
  }

  clio::run::u64 total_bytes_;
  clio::run::u64 block_bytes_;
  double spin_wait_ms_;
  int prefetch_distance_;
  int num_blocks_;
};

namespace {

void WriteConfig(const std::string &path, const std::string &disk_path,
                 clio::run::u64 ram_capacity, clio::run::u64 disk_capacity) {
  std::ofstream f(path);
  f << R"(
runtime:
  num_threads: 4
  queue_depth: 1024
  first_busy_wait: 10000
  max_sleep: 50000

compose:
  - mod_name: clio_cte_core
    pool_name: clio_cte
    pool_query: local
    pool_id: 512.0

    targets:
      neighborhood: 1
      default_target_timeout_ms: 30000
      poll_period_ms: 5000

    storage:
      - path: "ram::prefetch_bench_dram"
        bdev_type: "ram"
        capacity_limit: ")" << ram_capacity << R"("
        score: )" << kFastTierScore << R"(

      - path: ")" << disk_path << R"("
        bdev_type: "file"
        capacity_limit: ")" << disk_capacity << R"("
        score: )" << kSlowTierScore << R"(

    dpe:
      dpe_type: "max_bw"

    organizer: "none"
)";
}

void PrintTrialTable(const char *label, const std::vector<TrialResult> &trials) {
  std::vector<double> totals;
  for (const auto &t : trials) totals.push_back(t.total_ms);
  double mean = Mean(totals);
  double sd = StdDev(totals, mean);
  double lo = *std::min_element(totals.begin(), totals.end());
  double hi = *std::max_element(totals.begin(), totals.end());

  std::vector<double> all_reads;
  for (const auto &t : trials)
    for (double r : t.read_latency_ms) all_reads.push_back(r);

  printf("\n=== %s ===\n", label);
  printf("  trials: %zu\n", trials.size());
  printf("  total time (ms): mean=%.2f sd=%.2f min=%.2f max=%.2f\n", mean, sd,
        lo, hi);
  printf("  per-block read latency (ms): mean=%.3f p50=%.3f p95=%.3f\n",
        Mean(all_reads), Percentile(all_reads, 0.5),
        Percentile(all_reads, 0.95));
  for (size_t i = 0; i < trials.size(); ++i) {
    printf("    trial %zu: total=%.2f ms\n", i, trials[i].total_ms);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr,
           "Usage: %s <total_mb> <block_mb> <spin_wait_ms> "
           "<prefetch_distance> <trials>\n",
           argv[0]);
    fprintf(stderr,
           "Example: %s 256 4 40 2 5\n", argv[0]);
    return 1;
  }

  clio::run::u64 total_mb = std::stoull(argv[1]);
  clio::run::u64 block_mb = std::stoull(argv[2]);
  double spin_wait_ms = std::stod(argv[3]);
  int prefetch_distance = std::atoi(argv[4]);
  int trials = std::atoi(argv[5]);

  if (total_mb == 0 || block_mb == 0 || trials <= 0 ||
      prefetch_distance < 0) {
    fprintf(stderr, "Invalid parameters\n");
    return 1;
  }

  clio::run::u64 total_bytes = total_mb * 1024ULL * 1024ULL;
  clio::run::u64 block_bytes = block_mb * 1024ULL * 1024ULL;

  std::string test_dir = TestDataDir();
  fs::create_directories(test_dir);
  std::string config_path = test_dir + "/prefetch_bench_config.yaml";
  std::string disk_storage_path = test_dir + "/prefetch_bench_disk.bin";
  if (fs::exists(disk_storage_path)) fs::remove(disk_storage_path);

  // Fast tier only needs to hold the prefetch window (few blocks); slow tier
  // must hold the whole dataset plus slack.
  clio::run::u64 ram_capacity =
      block_bytes * static_cast<clio::run::u64>(std::max(prefetch_distance, 1) + 3);
  clio::run::u64 disk_capacity = total_bytes + block_bytes * 4;

  WriteConfig(config_path, disk_storage_path, ram_capacity, disk_capacity);
  ctp::SystemInfo::Setenv("CLIO_SERVER_CONF", config_path.c_str(), 1);

  printf("Initializing Clio runtime (embedded)...\n");
  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, true)) {
    fprintf(stderr, "Failed to initialize Clio runtime\n");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  if (!clio::cte::core::CLIO_CTE_CLIENT_INIT()) {
    fprintf(stderr, "Failed to initialize CTE client\n");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  PrefetchBenchmark bench(total_bytes, block_bytes, spin_wait_ms,
                          prefetch_distance);

  printf("=== CLIO Prefetch Benchmark ===\n");
  printf("total=%llu MB block=%llu MB blocks=%d spin_wait=%.1f ms "
        "prefetch_distance=%d trials=%d\n",
        static_cast<unsigned long long>(total_mb),
        static_cast<unsigned long long>(block_mb), bench.num_blocks(),
        spin_wait_ms, prefetch_distance, trials);
  printf("ram_capacity=%llu MB disk_capacity=%llu MB\n",
        static_cast<unsigned long long>(ram_capacity / (1024 * 1024)),
        static_cast<unsigned long long>(disk_capacity / (1024 * 1024)));

  std::vector<TrialResult> baseline_trials;
  std::vector<TrialResult> prefetch_trials;

  for (int t = 0; t < trials; ++t) {
    std::string tag_name = "baseline_trial_" + std::to_string(t);
    bench.ResetDataset(tag_name);
    baseline_trials.push_back(bench.RunTrial(tag_name, /*prefetch=*/false));
    bench.CleanupDataset(tag_name);
  }

  for (int t = 0; t < trials; ++t) {
    std::string tag_name = "prefetch_trial_" + std::to_string(t);
    bench.ResetDataset(tag_name);
    prefetch_trials.push_back(bench.RunTrial(tag_name, /*prefetch=*/true));
    bench.CleanupDataset(tag_name);
  }

  PrintTrialTable("BASELINE (no prefetch)", baseline_trials);
  PrintTrialTable("PREFETCH (ReorganizeBlob ahead)", prefetch_trials);

  std::vector<double> base_totals, pre_totals;
  for (auto &t : baseline_trials) base_totals.push_back(t.total_ms);
  for (auto &t : prefetch_trials) pre_totals.push_back(t.total_ms);
  double base_mean = Mean(base_totals);
  double pre_mean = Mean(pre_totals);

  printf("\n=== SUMMARY ===\n");
  printf("baseline mean total: %.2f ms\n", base_mean);
  printf("prefetch mean total: %.2f ms\n", pre_mean);
  if (pre_mean > 0.0) {
    printf("speedup (baseline/prefetch): %.3fx\n", base_mean / pre_mean);
  }

  return 0;
}

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
 * CTE DLIO-style Prefetch Benchmark (Benchmark 2, semester research task,
 * week of 2026-09-28).
 *
 * Models the DLIO (Deep Learning I/O) benchmark suite's published UNet3D
 * workload (dlio_benchmark 2.0.0, configs/workload/unet3d_v100.yaml):
 *   - 168 training files, one sample per file
 *   - record_length: mean 146,600,628 bytes, stdev 68,341,808 bytes
 *   - batch_size: 4, per-epoch file shuffle (file_shuffle: seed)
 *   - computation_time: 1.3604 s per training step (one step per batch)
 * These are the REAL published parameters, not invented. Two deliberate,
 * documented deviations from the reference config for this local run:
 *   - record_length is scaled by kSizeScale (see below) to fit this
 *     machine's free disk space; file count, batch size, computation time,
 *     and shuffle behavior are unscaled.
 *   - epoch/trial counts are set for a tractable interactive run, not the
 *     reference config's 5 epochs.
 *
 * This does NOT run the actual dlio_benchmark Python/PyTorch harness (that
 * would need the project's existing containerized Jarvis pipeline,
 * jarvis_clio_core/pipelines/local/dlio_benchmark_cte.yaml -- Docker, a FUSE
 * mount, and torch/tensorflow/CUDA-only deps that do not install on this
 * Mac). Instead it drives the same CTE client API as Benchmark 1
 * (AsyncPutBlob / AsyncGetBlob / AsyncReorganizeBlob) directly against a
 * dataset shaped like the real workload, which is what Luke asked for this
 * week ("run essentially the same test as (1)").
 *
 * Per-epoch access order is shuffled (matching file_shuffle: seed) and is
 * IDENTICAL between the baseline and prefetch runs of the same trial/epoch,
 * so the comparison is apples-to-apples. The prefetcher knows the
 * already-determined shuffled order for the CURRENT epoch (exactly as a
 * real PyTorch DataLoader would, since the sampler decides the epoch's
 * order before iteration starts) and uses it to promote the NEXT batch's
 * files to DRAM while the current batch's simulated training step runs.
 *
 * Usage:
 *   clio_dlio_prefetch_bench <epochs> <trials>
 *
 * Example:
 *   clio_dlio_prefetch_bench 2 3
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
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {

// --- Real dlio_benchmark 2.0.0 unet3d_v100.yaml parameters -----------------
constexpr int kNumFiles = 168;
constexpr int kBatchSize = 4;
constexpr double kComputationTimeSec = 1.3604;
constexpr clio::run::u64 kRecordLengthMean = 146600628ULL;
constexpr clio::run::u64 kRecordLengthStdev = 68341808ULL;

// --- Local, documented deviations -------------------------------------------
// Scales record_length (and its stdev) down to fit this machine's free disk
// space (~36 GB at time of writing; full-scale dataset would be ~24.6 GB per
// COPY and this benchmark needs headroom for the disk tier + DRAM window).
constexpr double kSizeScale = 0.25;
constexpr clio::run::u64 kMinRecordLength = 1024ULL * 1024;  // floor, 1 MB

constexpr float kFastTierScore = 1.0f;  // DRAM
constexpr float kSlowTierScore = 0.1f;  // disk
constexpr unsigned kShuffleSeedBase = 1337;  // fixed base -> reproducible

std::string TestDataDir() {
  const char *d = std::getenv("CLIO_DLIO_BENCH_DIR");
  return (d && *d) ? d : "/tmp/clio_dlio_prefetch_bench";
}

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

std::string FileName(int i) { return "unet3d_sample_" + std::to_string(i); }

/** Deterministic per-file size: sampled once (same for baseline & prefetch,
 *  same across trials) from N(mean, stdev), scaled, clamped to a floor. */
std::vector<clio::run::u64> GenerateFileSizes() {
  std::mt19937 rng(4242);  // fixed seed: same file-size layout every run
  std::normal_distribution<double> dist(
      static_cast<double>(kRecordLengthMean) * kSizeScale,
      static_cast<double>(kRecordLengthStdev) * kSizeScale);
  std::vector<clio::run::u64> sizes(kNumFiles);
  for (int i = 0; i < kNumFiles; ++i) {
    double v = dist(rng);
    if (v < static_cast<double>(kMinRecordLength)) v = static_cast<double>(kMinRecordLength);
    sizes[i] = static_cast<clio::run::u64>(v);
  }
  return sizes;
}

/** Per-epoch shuffled file order (file_shuffle: seed) -- one fixed sequence
 *  of orderings, reused identically by baseline and prefetch runs. */
std::vector<std::vector<int>> GenerateEpochOrders(int epochs) {
  std::vector<std::vector<int>> orders;
  for (int e = 0; e < epochs; ++e) {
    std::vector<int> order(kNumFiles);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(kShuffleSeedBase + static_cast<unsigned>(e));
    std::shuffle(order.begin(), order.end(), rng);
    orders.push_back(std::move(order));
  }
  return orders;
}

}  // namespace

struct EpochResult {
  double total_ms = 0.0;
  std::vector<double> batch_read_ms;  // wall time to read one batch (4 files)
};

class DlioPrefetchBenchmark {
 public:
  DlioPrefetchBenchmark(std::vector<clio::run::u64> sizes,
                        std::vector<std::vector<int>> epoch_orders)
      : sizes_(std::move(sizes)), epoch_orders_(std::move(epoch_orders)) {
    max_size_ = *std::max_element(sizes_.begin(), sizes_.end());
  }

  /** Put every file on the slow (disk) tier. Not timed. */
  void ResetDataset(const std::string &tag_name) {
    auto buf = CLIO_IPC->AllocateBuffer(max_size_);
    clio::cte::core::Tag tag(tag_name);
    for (int i = 0; i < kNumFiles; ++i) {
      std::memset(buf.ptr_, PatternFor(i), sizes_[i]);
      auto put = tag.AsyncPutBlob(FileName(i), buf.shm_.template Cast<void>(),
                                  sizes_[i], 0, kSlowTierScore);
      put.Wait();
    }
    CLIO_IPC->FreeBuffer(buf);
  }

  EpochResult RunEpoch(const std::string &tag_name, int epoch_idx,
                       bool prefetch_enabled) {
    auto *cte_client = CLIO_CTE_CLIENT;
    clio::cte::core::Tag tag(tag_name);
    clio::cte::core::TagId tag_id = tag.GetTagId();
    const std::vector<int> &order = epoch_orders_[epoch_idx];
    int num_batches = (kNumFiles + kBatchSize - 1) / kBatchSize;

    EpochResult result;
    result.batch_read_ms.reserve(num_batches);

    auto read_buf = CLIO_IPC->AllocateBuffer(max_size_);
    auto epoch_start = high_resolution_clock::now();

    for (int b = 0; b < num_batches; ++b) {
      int lo = b * kBatchSize;
      int hi = std::min(lo + kBatchSize, kNumFiles);

      auto t0 = high_resolution_clock::now();
      std::vector<clio::run::Future<clio::cte::core::GetBlobTask>> gets;
      for (int k = lo; k < hi; ++k) {
        int file_idx = order[k];
        gets.push_back(cte_client->AsyncGetBlob(
            tag_id, FileName(file_idx), 0, sizes_[file_idx], 0,
            read_buf.shm_.template Cast<void>()));
      }
      for (auto &g : gets) g.Wait();
      auto t1 = high_resolution_clock::now();
      result.batch_read_ms.push_back(
          duration_cast<duration<double, std::milli>>(t1 - t0).count());

      for (int k = lo; k < hi; ++k) {
        int file_idx = order[k];
        if (gets[k - lo]->GetReturnCode() != 0) {
          std::cerr << "  WARNING: read of " << FileName(file_idx)
                    << " failed\n";
        }
      }

      if (prefetch_enabled) {
        // DIAGNOSTIC ORDERING (see OPEN_QUESTIONS.md Q-001): demote the
        // batch just consumed and WAIT for it to complete before promoting
        // the next batch, instead of firing both concurrently. This
        // sacrifices some overlap but isolates whether concurrent
        // alloc/free churn on the DRAM tier (vs. raw capacity) is what
        // triggers ReorganizeBlob's allocation-failure/data-loss path.
        std::vector<clio::run::Future<clio::cte::core::ReorganizeBlobTask>>
            demotes;
        for (int k = lo; k < hi; ++k) {
          demotes.push_back(cte_client->AsyncReorganizeBlob(
              tag_id, FileName(order[k]), kSlowTierScore));
        }
        for (auto &d : demotes) d.Wait();

        // Promote the NEXT batch (already-known shuffled order, exactly as
        // a real DataLoader's sampler would have it) to DRAM, fire-and-
        // forget, while the simulated training step below runs.
        int next_lo = hi, next_hi = std::min(hi + kBatchSize, kNumFiles);
        for (int k = next_lo; k < next_hi; ++k) {
          cte_client->AsyncReorganizeBlob(tag_id, FileName(order[k]),
                                          kFastTierScore);
        }
      }

      // Simulated training step (forward+backward pass) for this batch.
      BusyWaitMs(kComputationTimeSec * 1000.0);
    }

    auto epoch_end = high_resolution_clock::now();
    result.total_ms =
        duration_cast<duration<double, std::milli>>(epoch_end - epoch_start)
            .count();

    CLIO_IPC->FreeBuffer(read_buf);
    return result;
  }

  void CleanupDataset(const std::string &tag_name) {
    auto *cte_client = CLIO_CTE_CLIENT;
    clio::cte::core::Tag tag(tag_name);
    clio::cte::core::TagId tag_id = tag.GetTagId();
    for (int i = 0; i < kNumFiles; ++i) {
      cte_client->AsyncDelBlob(tag_id, FileName(i)).Wait();
    }
    cte_client->AsyncDelTag(tag_name).Wait();
  }

  clio::run::u64 TotalDatasetBytes() const {
    clio::run::u64 sum = 0;
    for (auto s : sizes_) sum += s;
    return sum;
  }

 private:
  static char PatternFor(int i) { return static_cast<char>(i & 0xFF); }

  std::vector<clio::run::u64> sizes_;
  std::vector<std::vector<int>> epoch_orders_;
  clio::run::u64 max_size_;
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
      - path: "ram::dlio_bench_dram"
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

void PrintEpochTable(const char *label,
                     const std::vector<std::vector<EpochResult>> &trials) {
  std::vector<double> totals;
  std::vector<double> all_batches;
  for (const auto &trial : trials) {
    double trial_total = 0.0;
    for (const auto &ep : trial) {
      trial_total += ep.total_ms;
      for (double b : ep.batch_read_ms) all_batches.push_back(b);
    }
    totals.push_back(trial_total);
  }
  double mean = Mean(totals);
  printf("\n=== %s ===\n", label);
  printf("  trials: %zu\n", trials.size());
  printf("  total time across all epochs (ms): mean=%.2f sd=%.2f\n", mean,
        StdDev(totals, mean));
  printf("  per-batch (4-file) read time (ms): mean=%.3f\n",
        Mean(all_batches));
  for (size_t i = 0; i < totals.size(); ++i) {
    printf("    trial %zu: total=%.2f ms\n", i, totals[i]);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <epochs> <trials>\n", argv[0]);
    fprintf(stderr, "Example: %s 2 3\n", argv[0]);
    return 1;
  }
  int epochs = std::atoi(argv[1]);
  int trials = std::atoi(argv[2]);
  if (epochs <= 0 || trials <= 0) {
    fprintf(stderr, "Invalid parameters\n");
    return 1;
  }

  std::string test_dir = TestDataDir();
  fs::create_directories(test_dir);
  std::string config_path = test_dir + "/dlio_bench_config.yaml";
  std::string disk_storage_path = test_dir + "/dlio_bench_disk.bin";
  if (fs::exists(disk_storage_path)) fs::remove(disk_storage_path);

  auto sizes = GenerateFileSizes();
  auto epoch_orders = GenerateEpochOrders(epochs);
  DlioPrefetchBenchmark bench(sizes, epoch_orders);

  clio::run::u64 total_bytes = bench.TotalDatasetBytes();
  clio::run::u64 max_size = *std::max_element(sizes.begin(), sizes.end());
  // Promote-next and demote-current are both fired async and can overlap in
  // flight (issue found during tuning: if a promote lands before the prior
  // batch's demote has actually freed DRAM space, ReorganizeBlob's
  // allocation can fail -- and on failure it currently DESTROYS the blob's
  // data instead of leaving it on its original tier; see OPEN_QUESTIONS.md).
  // Size generously (worst case ~2*batch_size files transiently in flight,
  // all at the sampled max size, plus margin) so this benchmark's own runs
  // don't hit that path, rather than papering over data loss with a retry.
  clio::run::u64 ram_capacity = max_size * (2 * kBatchSize + 6);
  clio::run::u64 disk_capacity = total_bytes + max_size * 4;

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

  printf("=== CLIO DLIO-style Prefetch Benchmark (UNet3D shape) ===\n");
  printf("files=%d batch_size=%d computation_time=%.4fs epochs=%d trials=%d\n",
        kNumFiles, kBatchSize, kComputationTimeSec, epochs, trials);
  printf("record_length: mean=%llu stdev=%llu (real config, scaled x%.2f)\n",
        static_cast<unsigned long long>(kRecordLengthMean),
        static_cast<unsigned long long>(kRecordLengthStdev), kSizeScale);
  printf("dataset total=%.2f GB ram_capacity=%.1f MB disk_capacity=%.2f GB\n",
        static_cast<double>(total_bytes) / 1e9,
        static_cast<double>(ram_capacity) / (1024.0 * 1024.0),
        static_cast<double>(disk_capacity) / 1e9);

  std::vector<std::vector<EpochResult>> baseline_trials, prefetch_trials;

  for (int t = 0; t < trials; ++t) {
    std::string tag_name = "dlio_baseline_" + std::to_string(t);
    bench.ResetDataset(tag_name);
    std::vector<EpochResult> eps;
    for (int e = 0; e < epochs; ++e) {
      eps.push_back(bench.RunEpoch(tag_name, e, /*prefetch=*/false));
    }
    baseline_trials.push_back(std::move(eps));
    bench.CleanupDataset(tag_name);
    printf("  baseline trial %d done\n", t);
  }

  for (int t = 0; t < trials; ++t) {
    std::string tag_name = "dlio_prefetch_" + std::to_string(t);
    bench.ResetDataset(tag_name);
    std::vector<EpochResult> eps;
    for (int e = 0; e < epochs; ++e) {
      eps.push_back(bench.RunEpoch(tag_name, e, /*prefetch=*/true));
    }
    prefetch_trials.push_back(std::move(eps));
    bench.CleanupDataset(tag_name);
    printf("  prefetch trial %d done\n", t);
  }

  PrintEpochTable("BASELINE (no prefetch)", baseline_trials);
  PrintEpochTable("PREFETCH (ReorganizeBlob ahead of next batch)",
                  prefetch_trials);

  std::vector<double> base_totals, pre_totals;
  for (auto &trial : baseline_trials) {
    double s = 0.0;
    for (auto &ep : trial) s += ep.total_ms;
    base_totals.push_back(s);
  }
  for (auto &trial : prefetch_trials) {
    double s = 0.0;
    for (auto &ep : trial) s += ep.total_ms;
    pre_totals.push_back(s);
  }
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

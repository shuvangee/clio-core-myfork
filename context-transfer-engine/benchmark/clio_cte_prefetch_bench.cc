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
 * Synthetic CTE client-side prefetch benchmark.
 *
 * The benchmark creates deterministic fixed-size blobs, forces them onto the
 * slow tier (score 0), and compares sequential reads with and without a
 * one-block look-ahead prefetch.  The prefetch is AsyncReorganizeBlob(score 1)
 * and runs while the client spin-waits to model application compute.
 * Population and per-trial demotion are setup and are not timed.  Trial wall
 * time starts before block 0's destination preparation and includes buffer
 * clearing, Get time, byte-for-byte verification, compute, and any residual
 * prefetch wait.  A block's read_latency_us measures only AsyncGetBlob
 * submission through completion; prefetch_wait_us is reported separately.
 */

#include "bench_common.h"

#include <clio_runtime/clio_runtime.h>
#include <clio_cte/core/core_client.h>
#include <clio_ctp/util/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono;

namespace {

constexpr float kSlowTierScore = 0.0f;
constexpr float kFastTierScore = 1.0f;

struct Args {
  clio::run::u64 total_size = 128ULL * 1024 * 1024;
  clio::run::u64 block_size = 4ULL * 1024 * 1024;
  clio::run::u64 compute_us = 5000;
  int trials = 5;
  std::string output;
  bool ok = false;
  bool help = false;
};

struct BlockResult {
  double read_latency_us = 0.0;
  double prefetch_wait_us = 0.0;
};

struct TrialResult {
  std::string mode;
  int trial = 0;
  double total_wall_ms = 0.0;
  std::vector<BlockResult> blocks;
};

void PrintUsage(const char *argv0) {
  std::cerr
      << "Usage: " << argv0
      << " [--total-size 128m] [--block-size 4m] [--compute-us 5000]"
         " [--trials 5] [--output results.csv]\n"
      << "  total-size must be an exact multiple of block-size.\n"
      << "  Set CLIO_BENCH_SELF_RUN=1 to launch an embedded runtime and\n"
      << "  CLIO_SERVER_CONF to a two-tier CTE configuration.\n";
}

Args ParseArgs(int argc, char **argv) {
  Args a;
  auto need = [&](int &i) -> const char * {
    if (++i >= argc) {
      std::cerr << "Missing value for " << argv[i - 1] << "\n";
      return nullptr;
    }
    return argv[i];
  };

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string flag = argv[i];
      if (flag == "--help" || flag == "-h") {
        a.help = true;
        return a;
      }
      const char *value = nullptr;
      if (flag == "--total-size") {
        if (!(value = need(i))) return a;
        a.total_size = clio_bench::ParseSize(value);
      } else if (flag == "--block-size") {
        if (!(value = need(i))) return a;
        a.block_size = clio_bench::ParseSize(value);
      } else if (flag == "--compute-us") {
        if (!(value = need(i))) return a;
        a.compute_us = std::stoull(value);
      } else if (flag == "--trials") {
        if (!(value = need(i))) return a;
        a.trials = std::stoi(value);
      } else if (flag == "--output") {
        if (!(value = need(i))) return a;
        a.output = value;
      } else {
        std::cerr << "Unknown option: " << flag << "\n";
        return a;
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Invalid argument: " << e.what() << "\n";
    return a;
  }

  if (a.total_size == 0 || a.block_size == 0 || a.trials <= 0 ||
      a.total_size % a.block_size != 0) {
    std::cerr << "total-size and block-size must be positive, total-size must "
                 "be divisible by block-size, and trials must be positive\n";
    return a;
  }
  a.ok = true;
  return a;
}

void BusyWait(clio::run::u64 wait_us) {
  const auto deadline = steady_clock::now() + microseconds(wait_us);
  while (steady_clock::now() < deadline) {
    // Prevent the compiler from reducing the loop to a sleep or eliminating it.
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
}

unsigned char ExpectedByte(std::size_t block, std::size_t offset) {
  return static_cast<unsigned char>(
      (block * 131ULL + offset * 17ULL + 0x5aULL) & 0xffULL);
}

void FillBlock(void *data, std::size_t size, std::size_t block) {
  auto *bytes = static_cast<unsigned char *>(data);
  for (std::size_t i = 0; i < size; ++i) bytes[i] = ExpectedByte(block, i);
}

bool VerifyBlock(const void *data, std::size_t size, std::size_t block,
                 std::size_t *bad_offset) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (std::size_t i = 0; i < size; ++i) {
    if (bytes[i] != ExpectedByte(block, i)) {
      *bad_offset = i;
      return false;
    }
  }
  return true;
}

std::string BlobName(std::size_t block) {
  return "block_" + std::to_string(block);
}

double Mean(const std::vector<double> &values) {
  if (values.empty()) return 0.0;
  return std::accumulate(values.begin(), values.end(), 0.0) /
         static_cast<double>(values.size());
}

class PrefetchBenchmark {
 public:
  explicit PrefetchBenchmark(Args args) : args_(std::move(args)) {
    block_count_ = static_cast<std::size_t>(args_.total_size / args_.block_size);
    const auto nonce = steady_clock::now().time_since_epoch().count();
    tag_name_ = "cte_prefetch_bench_" + std::to_string(nonce);
  }

  bool Run() {
    auto *cte = CLIO_CTE_CLIENT;
    auto tag_task = cte->AsyncGetOrCreateTag(tag_name_);
    tag_task.Wait();
    if (tag_task->GetReturnCode() != 0) {
      HLOG(kError, "GetOrCreateTag failed rc={}", tag_task->GetReturnCode());
      return false;
    }
    tag_id_ = tag_task->tag_id_;
    tag_created_ = true;

    put_buffer_ = CLIO_IPC->AllocateBuffer(args_.block_size);
    read_buffer_ = CLIO_IPC->AllocateBuffer(args_.block_size);
    if (put_buffer_.IsNull() || read_buffer_.IsNull()) {
      HLOG(kError, "Failed to allocate benchmark shared-memory buffers");
      if (!put_buffer_.IsNull()) CLIO_IPC->FreeBuffer(put_buffer_);
      if (!read_buffer_.IsNull()) CLIO_IPC->FreeBuffer(read_buffer_);
      Cleanup();
      return false;
    }
    buffers_allocated_ = true;

    HLOG(kInfo,
         "CTE prefetch benchmark: {} blocks x {}, compute={} us, trials={}",
         block_count_, clio_bench::FormatSize(args_.block_size),
         args_.compute_us, args_.trials);
    HLOG(kInfo, "Scores: slow={} (disk), fast={} (DRAM)", kSlowTierScore,
         kFastTierScore);

    if (!Populate()) {
      Cleanup();
      return false;
    }

    std::ofstream output_file;
    std::ostream *csv = &std::cout;
    if (!args_.output.empty()) {
      output_file.open(args_.output, std::ios::out | std::ios::trunc);
      if (!output_file) {
        HLOG(kError, "Could not open output file {}", args_.output);
        Cleanup();
        return false;
      }
      csv = &output_file;
    }
    *csv << "record,mode,trial,block,read_latency_us,prefetch_wait_us,"
            "total_wall_ms,correct\n";
    csv->setf(std::ios::fixed);
    *csv << std::setprecision(3);

    bool success = true;
    std::vector<TrialResult> results;
    results.reserve(static_cast<std::size_t>(args_.trials) * 2);
    for (int trial = 1; trial <= args_.trials && success; ++trial) {
      // Counterbalance mode order so a systematic first/second-run effect is
      // not attributed to prefetching.  Every mode still gets a fresh demotion.
      const bool prefetch_first = trial % 2 == 0;
      for (int mode_index = 0; mode_index < 2; ++mode_index) {
        const bool prefetch = (mode_index == 0) ? prefetch_first
                                                : !prefetch_first;
        if (!DemoteAll()) {
          success = false;
          break;
        }
        TrialResult result;
        if (!RunTrial(prefetch, trial, &result)) {
          success = false;
          break;
        }
        WriteTrial(*csv, result);
        results.push_back(std::move(result));
      }
    }
    csv->flush();
    if (!*csv) {
      HLOG(kError, "Failed while writing benchmark CSV output");
      success = false;
    }
    if (success) PrintSummary(results);
    const bool cleanup_ok = Cleanup();
    return success && cleanup_ok;
  }

 private:
  bool Populate() {
    auto *cte = CLIO_CTE_CLIENT;
    const auto put_ptr = put_buffer_.shm_.template Cast<void>();
    for (std::size_t block = 0; block < block_count_; ++block) {
      FillBlock(put_buffer_.ptr_, static_cast<std::size_t>(args_.block_size),
                block);
      auto task = cte->AsyncPutBlob(tag_id_, BlobName(block), 0,
                                    args_.block_size, put_ptr,
                                    kFastTierScore);
      task.Wait();
      if (task->GetReturnCode() != 0) {
        HLOG(kError, "PutBlob block {} failed rc={}", block,
             task->GetReturnCode());
        return false;
      }
      populated_blocks_ = block + 1;
    }
    return true;
  }

  bool DemoteAll() {
    auto *cte = CLIO_CTE_CLIENT;
    std::vector<clio::run::Future<clio::cte::core::ReorganizeBlobTask>> tasks;
    tasks.reserve(block_count_);
    for (std::size_t block = 0; block < block_count_; ++block) {
      tasks.push_back(cte->AsyncReorganizeBlob(
          tag_id_, BlobName(block), kSlowTierScore));
    }
    for (std::size_t block = 0; block < tasks.size(); ++block) {
      tasks[block].Wait();
      if (tasks[block]->GetReturnCode() != 0) {
        HLOG(kError, "Demotion of block {} failed rc={}", block,
             tasks[block]->GetReturnCode());
        return false;
      }
    }
    return true;
  }

  bool RunTrial(bool prefetch, int trial, TrialResult *result) {
    auto *cte = CLIO_CTE_CLIENT;
    const auto read_ptr = read_buffer_.shm_.template Cast<void>();
    result->mode = prefetch ? "prefetch" : "baseline";
    result->trial = trial;
    result->blocks.reserve(block_count_);

    std::vector<clio::run::Future<clio::cte::core::ReorganizeBlobTask>> pending;
    pending.reserve(1);
    const auto wall_start = steady_clock::now();
    for (std::size_t block = 0; block < block_count_; ++block) {
      double prefetch_wait_us = 0.0;
      if (!pending.empty()) {
        const auto wait_start = steady_clock::now();
        pending.back().Wait();
        prefetch_wait_us = duration<double, std::micro>(
                               steady_clock::now() - wait_start)
                               .count();
        if (pending.back()->GetReturnCode() != 0) {
          HLOG(kError, "Prefetch of block {} failed rc={}", block,
               pending.back()->GetReturnCode());
          return false;
        }
        pending.clear();
      }

      std::memset(read_buffer_.ptr_, 0,
                  static_cast<std::size_t>(args_.block_size));
      const auto read_start = steady_clock::now();
      auto get = cte->AsyncGetBlob(tag_id_, BlobName(block), 0,
                                   args_.block_size, 0, read_ptr);
      get.Wait();
      const double read_latency_us =
          duration<double, std::micro>(steady_clock::now() - read_start)
              .count();
      if (get->GetReturnCode() != 0) {
        HLOG(kError, "GetBlob block {} failed rc={}", block,
             get->GetReturnCode());
        return false;
      }
      std::size_t bad_offset = 0;
      if (!VerifyBlock(read_buffer_.ptr_,
                       static_cast<std::size_t>(args_.block_size), block,
                       &bad_offset)) {
        HLOG(kError,
             "Data mismatch: mode={} trial={} block={} offset={} got={} "
             "expected={}",
             result->mode, trial, block, bad_offset,
             static_cast<unsigned int>(reinterpret_cast<unsigned char *>(
                 read_buffer_.ptr_)[bad_offset]),
             static_cast<unsigned int>(ExpectedByte(block, bad_offset)));
        return false;
      }
      result->blocks.push_back({read_latency_us, prefetch_wait_us});

      if (block + 1 < block_count_) {
        if (prefetch) {
          // Fire-and-overlap: completion is checked immediately before the
          // next Get, after the same compute interval used by the baseline.
          pending.push_back(cte->AsyncReorganizeBlob(
              tag_id_, BlobName(block + 1), kFastTierScore));
        }
        BusyWait(args_.compute_us);
      }
    }
    result->total_wall_ms =
        duration<double, std::milli>(steady_clock::now() - wall_start).count();
    return true;
  }

  static void WriteTrial(std::ostream &csv, const TrialResult &result) {
    for (std::size_t block = 0; block < result.blocks.size(); ++block) {
      csv << "block," << result.mode << ',' << result.trial << ',' << block
          << ',' << result.blocks[block].read_latency_us << ','
          << result.blocks[block].prefetch_wait_us << ",,true\n";
    }
    csv << "trial," << result.mode << ',' << result.trial << ",,,,"
        << result.total_wall_ms << ",true\n";
  }

  static void PrintSummary(const std::vector<TrialResult> &results) {
    for (const std::string mode : {"baseline", "prefetch"}) {
      std::vector<double> all_reads;
      std::vector<double> upcoming_reads;
      std::vector<double> walls;
      std::vector<double> waits;
      for (const auto &result : results) {
        if (result.mode != mode) continue;
        walls.push_back(result.total_wall_ms);
        for (std::size_t i = 0; i < result.blocks.size(); ++i) {
          all_reads.push_back(result.blocks[i].read_latency_us);
          waits.push_back(result.blocks[i].prefetch_wait_us);
          if (i > 0) upcoming_reads.push_back(result.blocks[i].read_latency_us);
        }
      }
      HLOG(kInfo,
           "SUMMARY mode={} trials={} mean_read_us={} "
           "mean_upcoming_read_us={} mean_prefetch_wait_us={} "
           "mean_total_wall_ms={}",
           mode, walls.size(), Mean(all_reads), Mean(upcoming_reads),
           Mean(waits), Mean(walls));
    }
  }

  bool Cleanup() {
    bool success = true;
    auto *cte = CLIO_CTE_CLIENT;
    if (tag_created_) {
      for (std::size_t block = 0; block < populated_blocks_; ++block) {
        auto task = cte->AsyncDelBlob(tag_id_, BlobName(block));
        task.Wait();
        if (task->GetReturnCode() != 0) {
          HLOG(kWarning, "Cleanup DelBlob block {} failed rc={}", block,
               task->GetReturnCode());
          success = false;
        }
      }
      auto task = cte->AsyncDelTag(tag_name_);
      task.Wait();
      if (task->GetReturnCode() != 0) {
        HLOG(kWarning, "Cleanup DelTag failed rc={}", task->GetReturnCode());
        success = false;
      }
      tag_created_ = false;
    }
    if (buffers_allocated_) {
      CLIO_IPC->FreeBuffer(put_buffer_);
      CLIO_IPC->FreeBuffer(read_buffer_);
      buffers_allocated_ = false;
    }
    return success;
  }

  Args args_;
  std::size_t block_count_ = 0;
  std::size_t populated_blocks_ = 0;
  std::string tag_name_;
  clio::cte::core::TagId tag_id_;
  ctp::ipc::FullPtr<char> put_buffer_;
  ctp::ipc::FullPtr<char> read_buffer_;
  bool tag_created_ = false;
  bool buffers_allocated_ = false;
};

}  // namespace

int main(int argc, char **argv) {
  Args args = ParseArgs(argc, argv);
  if (args.help) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (!args.ok) {
    PrintUsage(argv[0]);
    return 1;
  }

  const char *self_run = std::getenv("CLIO_BENCH_SELF_RUN");
  const bool embed = self_run != nullptr && self_run[0] == '1';
  HLOG(kInfo, "Initializing Clio runtime (embedded={})", embed);
  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, embed)) {
    HLOG(kError, "Failed to initialize Clio runtime");
    return 1;
  }
  struct ClientFinalizeGuard {
    ~ClientFinalizeGuard() {
      auto *mgr = CLIO_RUNTIME_MANAGER;
      if (mgr) mgr->ClientFinalize();
    }
  } finalize_guard;

  std::this_thread::sleep_for(milliseconds(500));
  if (!clio::cte::core::CLIO_CTE_CLIENT_INIT()) {
    HLOG(kError, "Failed to initialize CTE client");
    return 1;
  }
  std::this_thread::sleep_for(milliseconds(200));

  PrefetchBenchmark benchmark(std::move(args));
  if (!benchmark.Run()) {
    HLOG(kError, "CTE prefetch benchmark failed");
    return 1;
  }
  return 0;
}

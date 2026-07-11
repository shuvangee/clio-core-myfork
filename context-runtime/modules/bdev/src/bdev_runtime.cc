/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 */

#include <clio_runtime/bdev/bdev_runtime.h>
#include <clio_runtime/comutex.h>
#include <clio_ctp/util/gpu_api.h>
#include <clio_runtime/work_orchestrator.h>
#include <clio_runtime/worker.h>
#include <clio_ctp/introspect/system_info.h>
#include <clio_ctp/serialize/msgpack_wrapper.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

#include "clio_ctp/util/timer.h"

namespace clio::run::bdev {

Runtime::~Runtime() { StopHealthPolling(); }

clio::run::TaskStat Runtime::GetTaskStats(const clio::run::Task *task) const {
  if (!task) return clio::run::TaskStat();
  switch (task->method_) {
    case Method::kWrite: {
      auto *wt = static_cast<const WriteTask *>(task);
      clio::run::TaskStat stat;
      stat.io_size_ = wt->length_;
      size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0f;
      return stat;
    }
    case Method::kRead: {
      auto *rt = static_cast<const ReadTask *>(task);
      clio::run::TaskStat stat;
      stat.io_size_ = rt->length_;
      size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0f;
      return stat;
    }
    default: return clio::run::TaskStat();
  }
}

size_t Runtime::GetWorkerID(clio::run::RunContext &rctx) {
  clio::run::Worker *worker = CLIO_CUR_WORKER;
  if (worker == nullptr) {
    return 0;
  }
  return worker->GetId();
}

clio::run::TaskResume Runtime::Create(clio::run::shared_ptr<CreateTask> &task) {
  CLIO_TASK_BODY_BEGIN

  CreateParams params = task->GetParams();
  bdev_type_ = params.bdev_type_;
  
  transport_ = BdevTransportFactory::Create(bdev_type_);
  if (!transport_) {
    if (bdev_type_ == BdevType::kNoop) {
      task->return_code_ = 0;
      CLIO_CO_RETURN;
    }
    HLOG(kError, "Failed to create bdev transport for type {}", static_cast<int>(bdev_type_));
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // pool_name doubles as the file path (kFile) / S3 bucket (kS3); it lives on
  // the create task, not in CreateParams.
  if (!transport_->Init(params, task->pool_name_.str(), this)) {
    HLOG(kError, "Failed to initialize bdev transport");
    task->return_code_ = 2;
    CLIO_CO_RETURN;
  }

  perf_metrics_ = params.perf_metrics_;
  start_time_ = std::chrono::high_resolution_clock::now();
  total_reads_ = 0;
  total_writes_ = 0;
  total_bytes_read_ = 0;
  total_bytes_written_ = 0;

  // Capture the device path so the poll thread can look up SMART attributes.
  device_path_ = task->pool_name_.str();

  // Start background health-poll thread.  It will periodically query the
  // local prediction server and update predicted_ttl_days_.
  health_poll_stop_.store(false, std::memory_order_relaxed);
  health_poll_thread_ = std::thread(&Runtime::PollPredictionServer, this);

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::AllocateBlocks(clio::run::shared_ptr<AllocateBlocksTask> &task) {
  CLIO_TASK_BODY_BEGIN

  if (bdev_type_ == BdevType::kNoop) {
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }

  clio::run::Worker *worker = CLIO_CUR_WORKER;
  int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId()) : 0;
  std::vector<Block> local_blocks;

  if (!transport_->AllocateBlocks(task->size_, worker_id, local_blocks)) {
    task->blocks_.clear();
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  for (const auto& block : local_blocks) {
    task->blocks_.push_back(block);
  }

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::FreeBlocks(clio::run::shared_ptr<FreeBlocksTask> &task) {
  CLIO_TASK_BODY_BEGIN

  if (bdev_type_ == BdevType::kNoop) {
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }

  clio::run::Worker *worker = CLIO_CUR_WORKER;
  int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId()) : 0;
  std::vector<Block> local_blocks;
  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    local_blocks.push_back(task->blocks_[i]);
  }

  transport_->FreeBlocks(worker_id, local_blocks);

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Write(clio::run::shared_ptr<WriteTask> &task) {
  CLIO_TASK_BODY_BEGIN

  if (bdev_type_ == BdevType::kNoop) {
    task->return_code_ = 0;
    task->bytes_written_ = task->length_;
    CLIO_CO_RETURN;
  }

  if (transport_) {
    CLIO_CO_AWAIT(transport_->WriteBlocks(ctp::ipc::FullPtr<WriteTask>(task.get())));
    total_writes_.fetch_add(1);
    total_bytes_written_.fetch_add(task->bytes_written_);
  } else {
    task->return_code_ = 1;
    task->bytes_written_ = 0;
  }

  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Read(clio::run::shared_ptr<ReadTask> &task) {
  CLIO_TASK_BODY_BEGIN

  if (bdev_type_ == BdevType::kNoop) {
    task->return_code_ = 0;
    task->bytes_read_ = task->length_;
    CLIO_CO_RETURN;
  }

  if (transport_) {
    CLIO_CO_AWAIT(transport_->ReadBlocks(ctp::ipc::FullPtr<ReadTask>(task.get())));
    total_reads_.fetch_add(1);
    total_bytes_read_.fetch_add(task->bytes_read_);
  } else {
    task->return_code_ = 1;
    task->bytes_read_ = 0;
  }

  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Update(clio::run::shared_ptr<UpdateTask> &task) {
  CLIO_TASK_BODY_BEGIN
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::GetStats(clio::run::shared_ptr<GetStatsTask> &task) {
  CLIO_TASK_BODY_BEGIN
  ReadTask r_synthetic;
  r_synthetic.method_ = Method::kRead;
  r_synthetic.length_ = 1024 * 1024;
  WriteTask w_synthetic;
  w_synthetic.method_ = Method::kWrite;
  w_synthetic.length_ = 1024 * 1024;
  clio::run::TaskStat read_stat = GetTaskStats(&r_synthetic);
  clio::run::TaskStat write_stat = GetTaskStats(&w_synthetic);
  float read_wall_us = InferWallClockTime(Method::kRead, read_stat);
  float write_wall_us = InferWallClockTime(Method::kWrite, write_stat);
  double read_size_mb = static_cast<double>(read_stat.io_size_) / (1024.0 * 1024.0);
  double write_size_mb = static_cast<double>(write_stat.io_size_) / (1024.0 * 1024.0);
  task->metrics_.read_bandwidth_mbps_ = (read_wall_us > 0)
      ? read_size_mb / (read_wall_us * 1e-6) : perf_metrics_.read_bandwidth_mbps_;
  task->metrics_.write_bandwidth_mbps_ = (write_wall_us > 0)
      ? write_size_mb / (write_wall_us * 1e-6) : perf_metrics_.write_bandwidth_mbps_;
  task->metrics_.read_latency_us_ = read_wall_us;
  task->metrics_.write_latency_us_ = write_wall_us;
  task->metrics_.iops_ = perf_metrics_.iops_;

  if (transport_) {
    task->remaining_size_ = transport_->GetRemainingSize();
  } else {
    task->remaining_size_ = 0;
  }

  // Expose the latest ML-predicted TTL so the CTE can make
  // TTL-aware allocation decisions without any external daemon.
  task->predicted_ttl_days_ = predicted_ttl_days_.load(std::memory_order_relaxed);

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

/**
 * Update the in-memory predicted lifespan that bdev reports to callers.
 *
 * @param task SetLifespan task carrying the new remaining-life estimate.
 * @return Task resume state for the CLIO scheduler.
 */
clio::run::TaskResume Runtime::SetLifespan(
    clio::run::shared_ptr<SetLifespanTask> &task) {
  CLIO_TASK_BODY_BEGIN
  predicted_ttl_days_.store(task->lifespan_days_, std::memory_order_relaxed);
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Destroy(clio::run::shared_ptr<DestroyTask> &task) {
  CLIO_TASK_BODY_BEGIN
  StopHealthPolling();
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::u64 Runtime::GetWorkRemaining() const { return 0; }

clio::run::TaskResume Runtime::Monitor(clio::run::shared_ptr<MonitorTask> &task) {
  CLIO_TASK_BODY_BEGIN
  if (task->query_ == "stats") {
    ReadTask r_synthetic;
    r_synthetic.method_ = Method::kRead;
    r_synthetic.length_ = 1024 * 1024;
    WriteTask w_synthetic;
    w_synthetic.method_ = Method::kWrite;
    w_synthetic.length_ = 1024 * 1024;
    clio::run::TaskStat read_stat = GetTaskStats(&r_synthetic);
    clio::run::TaskStat write_stat = GetTaskStats(&w_synthetic);
    float read_wall_us = InferWallClockTime(Method::kRead, read_stat);
    float write_wall_us = InferWallClockTime(Method::kWrite, write_stat);
    double read_size_mb = static_cast<double>(read_stat.io_size_) / (1024.0 * 1024.0);
    double write_size_mb = static_cast<double>(write_stat.io_size_) / (1024.0 * 1024.0);
    double read_bw = (read_wall_us > 0)
        ? read_size_mb / (read_wall_us * 1e-6) : perf_metrics_.read_bandwidth_mbps_;
    double write_bw = (write_wall_us > 0)
        ? write_size_mb / (write_wall_us * 1e-6) : perf_metrics_.write_bandwidth_mbps_;

    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);

    pk.pack_map(16);
    pk.pack("pool_name");              pk.pack(pool_name_);
    pk.pack("bdev_type");              pk.pack(static_cast<clio::run::u32>(bdev_type_));
    pk.pack("total_capacity");         pk.pack(transport_ ? transport_->GetCapacity() : 0);
    pk.pack("remaining_capacity");     pk.pack(transport_ ? transport_->GetRemainingSize() : 0);
    pk.pack("read_bandwidth_mbps");    pk.pack(read_bw);
    pk.pack("write_bandwidth_mbps");   pk.pack(write_bw);
    pk.pack("read_latency_us");        pk.pack(static_cast<double>(read_wall_us));
    pk.pack("write_latency_us");       pk.pack(static_cast<double>(write_wall_us));
    pk.pack("iops");                   pk.pack(perf_metrics_.iops_);
    pk.pack("total_reads");            pk.pack(total_reads_.load());
    pk.pack("total_writes");           pk.pack(total_writes_.load());
    pk.pack("total_bytes_read");       pk.pack(total_bytes_read_.load());
    pk.pack("total_bytes_written");    pk.pack(total_bytes_written_.load());

    std::string health_json = ctp::SystemInfo::GetDeviceHealthStats(pool_name_);
    std::string drive_type = ctp::SystemInfo::DeriveDriveType(pool_name_);
    std::string prediction_json = ctp::SystemInfo::PredictDriveFailure(drive_type, health_json, pool_name_);

    pk.pack("device_health");          pk.pack(health_json);
    pk.pack("failure_prediction");     pk.pack(prediction_json);
    pk.pack("predicted_ttl_days");     pk.pack(predicted_ttl_days_.load(std::memory_order_relaxed));

    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  }
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

void Runtime::PostGpuContainerCreate() {}

void Runtime::StopHealthPolling() {
  health_poll_stop_.store(true, std::memory_order_relaxed);
  if (health_poll_thread_.joinable()) {
    health_poll_thread_.join();
  }
  if (transport_) {
    transport_->Destroy();
    transport_.reset();
  }
}

// ---------------------------------------------------------------------------
// PollPredictionServer
//
// Background thread body.  Every kHealthPollIntervalSec seconds it:
//   1. Reads SMART attributes for this device via SystemInfo.
//   2. Posts them to the local prediction server (server.py).
//   3. Parses "days_remaining" from the JSON response.
//   4. Stores the result in predicted_ttl_days_ so GetStats() can expose it
//      to the CTE on the next poll without any external daemon involved.
// ---------------------------------------------------------------------------
void Runtime::PollPredictionServer() {
  // Derive the drive type string ("hdd" or "ssd") from the pool / device name.
  const std::string drive_type = ctp::SystemInfo::DeriveDriveType(pool_name_);

  while (!health_poll_stop_.load(std::memory_order_relaxed)) {
    try {
      // Step 1: collect SMART attributes.
      const std::string health_json =
          ctp::SystemInfo::GetDeviceHealthStats(pool_name_);

      // Step 2 & 3: post to prediction server and get back a JSON response
      // that contains "days_remaining": <int> | null.
      const std::string pred_json =
          ctp::SystemInfo::PredictDriveFailure(drive_type, health_json, pool_name_);

      // Parse days_remaining out of the JSON string.  We use a lightweight
      // hand-rolled search to avoid pulling in a full JSON library here.
      //  • "days_remaining": 42   → 42
      //  • "days_remaining": null → keep current value (healthy)
      auto extract_days = [](const std::string &json) -> clio::run::u32 {
        const std::string key = "\"days_remaining\"";
        auto pos = json.find(key);
        if (pos == std::string::npos) return 999999;
        pos += key.size();
        // Skip whitespace and colon
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        // "null" means the server says the drive is healthy
        if (json.size() >= pos + 4 && json.substr(pos, 4) == "null") return 999999;
        // Parse integer
        if (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
          clio::run::u32 days = 0;
          while (pos < json.size() &&
                 std::isdigit(static_cast<unsigned char>(json[pos]))) {
            days = days * 10 + static_cast<clio::run::u32>(json[pos] - '0');
            ++pos;
          }
          return days;
        }
        return 999999;  // Unparseable → treat as healthy
      };

      clio::run::u32 new_ttl = extract_days(pred_json);
      predicted_ttl_days_.store(new_ttl, std::memory_order_relaxed);

    } catch (...) {
      // If the prediction server is unreachable, silently retain the last
      // known TTL rather than falsely marking the drive as dead.
    }

    // Sleep in 1-second increments so we can wake promptly on stop.
    for (unsigned s = 0;
         s < kHealthPollIntervalSec &&
         !health_poll_stop_.load(std::memory_order_relaxed);
         ++s) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

}  // namespace clio::run::bdev

// Define ChiMod entry points using CLIO_TASK_CC macro
CLIO_TASK_CC(clio::run::bdev::Runtime)

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

#include <clio_runtime/safe_bdev/safe_bdev_runtime.h>

#include <clio_ctp/serialize/msgpack_wrapper.h>

#include <cstring>
#include <string>
#include <vector>

namespace clio::run::safe_bdev {

//===========================================================================
// Helpers
//===========================================================================

chi::TaskStat Runtime::GetTaskStats(const chi::Task *task) const {
  if (task == nullptr) {
    return chi::TaskStat();
  }
  switch (task->method_) {
    case Method::kWrite: {
      const auto *wt = static_cast<const WriteTask *>(task);
      chi::TaskStat stat;
      stat.io_size_ = wt->length_;
      const size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0F;
      return stat;
    }
    case Method::kRead: {
      const auto *rt = static_cast<const ReadTask *>(task);
      chi::TaskStat stat;
      stat.io_size_ = rt->length_;
      const size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0F;
      return stat;
    }
    default:
      return chi::TaskStat();
  }
}

chi::priv::vector<clio::run::bdev::Block> Runtime::ShardBlocks(
    const MemberSlot &m, chi::u64 stripe) const {
  chi::priv::vector<clio::run::bdev::Block> blocks(CTP_MALLOC);
  const chi::u64 off = m.base_offset_ + stripe * kShardLen;
  blocks.push_back(clio::run::bdev::Block(off, kShardLen, 0));
  return blocks;
}

//===========================================================================
// EC fan-out helpers (run inside task fibers; co_await member bdev I/O)
//===========================================================================

chi::TaskResume Runtime::WriteShard(size_t mi, chi::u64 stripe,
                                    const uint8_t *src, chi::RunContext &ctx,
                                    bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kShardLen);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  std::memcpy(buf.ptr_, src, kShardLen);
  MemberSlot &m = members_[mi];
  auto fut = member_clients_[mi].AsyncWrite(
      MemberQuery(), ShardBlocks(m, stripe),
      buf.shm_.template Cast<void>(), kShardLen);
  CLIO_CO_AWAIT(fut);
  ok = (fut->return_code_ == 0) && (fut->bytes_written_ == kShardLen);
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::ReadShard(size_t mi, chi::u64 stripe, uint8_t *dst,
                                   chi::RunContext &ctx, bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kShardLen);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  MemberSlot &m = members_[mi];
  auto fut = member_clients_[mi].AsyncRead(
      MemberQuery(), ShardBlocks(m, stripe),
      buf.shm_.template Cast<void>(), kShardLen);
  CLIO_CO_AWAIT(fut);
  if (fut->return_code_ == 0 && fut->bytes_read_ == kShardLen) {
    std::memcpy(dst, buf.ptr_, kShardLen);
    ok = true;
  }
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::ReconstructStripe(
    chi::u64 stripe, int exclude, chi::RunContext &ctx,
    std::vector<std::vector<uint8_t>> &out, bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  // Gather up to k active survivors (data + parity), excluding `exclude`.
  std::vector<int> survivor_index;
  std::vector<std::vector<uint8_t>> survivor_buf;
  for (size_t i = 0; i < members_.size(); ++i) {
    if (static_cast<int>(i) == exclude) {
      continue;
    }
    if (members_[i].state_ != ec::EcState::kActive) {
      continue;
    }
    std::vector<uint8_t> buf(kShardLen, 0);
    bool rd_ok = false;
    CLIO_CO_AWAIT(ReadShard(i, stripe, buf.data(), rctx, rd_ok));
    if (!rd_ok) {
      CLIO_CO_RETURN;
    }
    survivor_index.push_back(GlobalShardIndex(members_[i]));
    survivor_buf.push_back(std::move(buf));
    if (static_cast<int>(survivor_index.size()) == k_) {
      break;
    }
  }
  if (static_cast<int>(survivor_index.size()) < k_) {
    CLIO_CO_RETURN;  // Too many failures to reconstruct.
  }
  std::vector<const uint8_t *> ptrs(survivor_buf.size());
  for (size_t i = 0; i < survivor_buf.size(); ++i) {
    ptrs[i] = survivor_buf[i].data();
  }
  ok = rs_->DecodeData(survivor_index, ptrs, kShardLen, &out);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

//===========================================================================
// Method handlers
//===========================================================================

chi::TaskResume Runtime::Create(ctp::ipc::FullPtr<CreateTask> task,
                                chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN

  // Get the creation parameters.
  CreateParams params = task->GetParams();

  max_failures_ = params.max_failures_;
  current_parity_level_ = 0;
  members_.clear();
  member_clients_.clear();
  logical_next_ = 0;

  // Initial members are DATA members; k = number supplied.
  k_ = static_cast<int>(params.members_.size());
  if (k_ <= 0) {
    HLOG(kError, "safe_bdev Create: no member bdevs supplied");
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Determine how many stripes we can reserve. Query the smallest member's
  // remaining capacity and carve a contiguous region of num_stripes*kShardLen.
  chi::u64 min_remaining = ~static_cast<chi::u64>(0);
  for (const auto &desc : params.members_) {
    member_clients_.emplace_back(desc.pool_id_);
    auto stats = member_clients_.back().AsyncGetStats();
    CLIO_CO_AWAIT(stats);
    if (stats->return_code_ != 0) {
      HLOG(kError, "safe_bdev Create: GetStats failed for member '{}'",
           desc.pool_name_);
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    if (stats->remaining_size_ < min_remaining) {
      min_remaining = stats->remaining_size_;
    }
  }
  num_stripes_ = min_remaining / kShardLen;
  if (num_stripes_ == 0) {
    HLOG(kError, "safe_bdev Create: member too small for even one stripe");
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Reserve a contiguous backing region on each member and seat it as a data
  // column. AllocateBlocks gives us the base offset for stripe 0.
  const chi::u64 region = num_stripes_ * kShardLen;
  int col = 0;
  for (const auto &desc : params.members_) {
    MemberSlot slot;
    slot.pool_id_ = desc.pool_id_;
    slot.pool_name_ = desc.pool_name_;
    slot.node_id_ = desc.node_id_;
    slot.role_ = ec::EcRole::kData;
    slot.state_ = ec::EcState::kActive;
    slot.index_ = col;
    auto alloc = member_clients_[col].AsyncAllocateBlocks(MemberQuery(), region);
    CLIO_CO_AWAIT(alloc);
    if (alloc->return_code_ != 0 || alloc->blocks_.size() == 0) {
      HLOG(kError, "safe_bdev Create: AllocateBlocks failed for member '{}'",
           desc.pool_name_);
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    slot.base_offset_ = alloc->blocks_[0].offset_;
    members_.push_back(slot);
    ++col;
  }

  rs_ = std::make_unique<ec::ReedSolomon>(k_, static_cast<int>(max_failures_));
  logical_capacity_ =
      static_cast<chi::u64>(k_) * kShardLen * num_stripes_;

  HLOG(kInfo,
       "safe_bdev Create: pool='{}', k={}, M={}, num_stripes={}, "
       "logical_capacity={}",
       task->pool_name_.str(), k_, max_failures_, num_stripes_,
       logical_capacity_);

  // Kick off the background parity builder. Write records dirty stripes off the
  // critical path; this periodic task converges them to full protection
  // (design.md B.6). An on-demand AsyncBuildParity(max_batch=0) is also
  // available as a durability barrier.
  client_.AsyncBuildParity(MemberQuery(), /*max_batch=*/0,
                           /*period_us=*/kBuildParityPeriodUs);

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::AllocateBlocks(
    ctp::ipc::FullPtr<AllocateBlocksTask> task, chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  (void)rctx;
  // Bump-allocate from the linear logical space. Round the request up to a
  // whole number of per-stripe-data units is not required; the logical offset
  // maps to (stripe, column, byte) per design.md B.2.
  const chi::u64 size = task->size_;
  if (logical_next_ + size > logical_capacity_) {
    HLOG(kError, "safe_bdev AllocateBlocks: out of logical capacity ({} + {} > {})",
         logical_next_, size, logical_capacity_);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  const chi::u64 offset = logical_next_;
  logical_next_ += size;
  task->blocks_.clear();
  task->blocks_.push_back(clio::run::bdev::Block(offset, size, 0));
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::FreeBlocks(ctp::ipc::FullPtr<FreeBlocksTask> task,
                                    chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  (void)rctx;
  // The simple bump allocator does not reclaim freed regions (sufficient for
  // the first-cut milestone; TODO(#543) free-list).
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::Write(ctp::ipc::FullPtr<WriteTask> task,
                               chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  if (rs_ == nullptr || task->blocks_.size() == 0) {
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> data = ipc->ToFullPtr(task->data_).template Cast<char>();
  const chi::u64 logical_base = task->blocks_[0].offset_;
  const chi::u64 len = task->length_;
  const chi::u64 per_stripe_data = static_cast<chi::u64>(k_) * kShardLen;

  // This first cut assumes stripe/shard-aligned, full-stripe writes (the test
  // writes aligned data). Parity is deferred to BuildParity (see below).
  if (logical_base % per_stripe_data != 0 || len % per_stripe_data != 0) {
    HLOG(kError,
         "safe_bdev Write: unaligned write (offset={}, len={}, stripe={}) "
         "not supported in first-cut milestone",
         logical_base, len, per_stripe_data);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  const chi::u64 first_stripe = logical_base / per_stripe_data;
  const chi::u64 n_stripes = len / per_stripe_data;
  chi::u64 bytes_written = 0;

  for (chi::u64 si = 0; si < n_stripes; ++si) {
    const chi::u64 s = first_stripe + si;
    // Slice the k data shards for this stripe out of the host buffer.
    std::vector<std::vector<uint8_t>> data_shards(
        static_cast<size_t>(k_), std::vector<uint8_t>(kShardLen, 0));
    for (int c = 0; c < k_; ++c) {
      const chi::u64 off = si * per_stripe_data + static_cast<chi::u64>(c) * kShardLen;
      std::memcpy(data_shards[c].data(), data.ptr_ + off, kShardLen);
    }

    // Write data shards to their data members in parallel.
    std::vector<chi::Future<WriteTask>> futs;
    std::vector<ctp::ipc::FullPtr<char>> bufs;
    bool dispatch_ok = true;
    for (int c = 0; c < k_; ++c) {
      MemberSlot &m = members_[static_cast<size_t>(c)];
      if (m.state_ != ec::EcState::kActive) {
        continue;
      }
      ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kShardLen);
      if (buf.IsNull()) {
        dispatch_ok = false;
        break;
      }
      std::memcpy(buf.ptr_, data_shards[c].data(), kShardLen);
      bufs.push_back(buf);
      futs.push_back(member_clients_[static_cast<size_t>(c)].AsyncWrite(
          MemberQuery(), ShardBlocks(m, s),
          buf.shm_.template Cast<void>(), kShardLen));
    }

    // Parity is deferred off the write path: once the data shards land, the
    // stripe is recorded dirty and BuildParity (on-demand or periodic) computes
    // parity asynchronously. This is the "no heavy write penalty" path — the
    // stripe is unprotected only during the bounded window before BuildParity
    // runs.
    bool stripe_ok = dispatch_ok;
    for (auto &f : futs) {
      CLIO_CO_AWAIT(f);
      if (f->return_code_ != 0 || f->bytes_written_ != kShardLen) {
        stripe_ok = false;
      }
    }
    for (auto &b : bufs) {
      ipc->FreeBuffer(b);
    }
    if (!stripe_ok) {
      task->bytes_written_ = bytes_written;
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    MarkStripeDirty(s);
    bytes_written += per_stripe_data;
  }

  task->bytes_written_ = bytes_written;
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::Read(ctp::ipc::FullPtr<ReadTask> task,
                              chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  if (rs_ == nullptr || task->blocks_.size() == 0) {
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> data = ipc->ToFullPtr(task->data_).template Cast<char>();
  const chi::u64 logical_base = task->blocks_[0].offset_;
  const chi::u64 len = task->length_;
  const chi::u64 per_stripe_data = static_cast<chi::u64>(k_) * kShardLen;

  if (logical_base % per_stripe_data != 0 || len % per_stripe_data != 0) {
    HLOG(kError,
         "safe_bdev Read: unaligned read (offset={}, len={}, stripe={}) "
         "not supported in first-cut milestone",
         logical_base, len, per_stripe_data);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  const chi::u64 first_stripe = logical_base / per_stripe_data;
  const chi::u64 n_stripes = len / per_stripe_data;
  chi::u64 bytes_read = 0;

  for (chi::u64 si = 0; si < n_stripes; ++si) {
    const chi::u64 s = first_stripe + si;
    // Are all data members active? Then plain parallel read; else reconstruct.
    bool all_data_active = true;
    for (int c = 0; c < k_; ++c) {
      if (members_[static_cast<size_t>(c)].state_ != ec::EcState::kActive) {
        all_data_active = false;
        break;
      }
    }

    std::vector<std::vector<uint8_t>> data_shards;
    bool stripe_ok = true;
    if (all_data_active) {
      data_shards.assign(static_cast<size_t>(k_),
                         std::vector<uint8_t>(kShardLen, 0));
      for (int c = 0; c < k_ && stripe_ok; ++c) {
        bool rd_ok = false;
        CLIO_CO_AWAIT(ReadShard(static_cast<size_t>(c), s,
                                data_shards[c].data(), rctx, rd_ok));
        stripe_ok = rd_ok;
      }
    } else if (IsStripeDirty(s)) {
      // A data member is down AND this stripe's parity is not yet current:
      // it is unprotected, so reconstruction would yield wrong bytes. Fail
      // loudly rather than return corrupt data.
      HLOG(kError,
           "safe_bdev Read: stripe {} is dirty (parity not built) and a data "
           "member is down — cannot reconstruct",
           s);
      stripe_ok = false;
    } else {
      // Degraded: reconstruct all data shards from survivors.
      CLIO_CO_AWAIT(ReconstructStripe(s, /*exclude=*/-1, rctx, data_shards,
                                      stripe_ok));
    }

    if (!stripe_ok) {
      task->bytes_read_ = bytes_read;
      task->length_ = bytes_read;
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }

    // Copy the k data shards out into the caller's buffer.
    for (int c = 0; c < k_; ++c) {
      const chi::u64 off = si * per_stripe_data + static_cast<chi::u64>(c) * kShardLen;
      std::memcpy(data.ptr_ + off, data_shards[c].data(), kShardLen);
    }
    bytes_read += per_stripe_data;
  }

  task->bytes_read_ = bytes_read;
  task->length_ = bytes_read;
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::GetStats(ctp::ipc::FullPtr<GetStatsTask> task,
                                  chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  // Aggregate remaining capacity across data members (parity is overhead and
  // is not counted toward usable logical capacity).
  chi::u64 remaining = (logical_capacity_ > logical_next_)
                           ? (logical_capacity_ - logical_next_)
                           : 0;
  task->remaining_size_ = remaining;
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::AddBdev(ctp::ipc::FullPtr<AddBdevTask> task,
                                 chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN

  const bool as_parity = (task->as_parity_ != 0);

  // Build a client for the new member and reserve its backing region.
  member_clients_.emplace_back(task->member_pool_id_);
  const size_t new_mi = member_clients_.size() - 1;
  const chi::u64 region = num_stripes_ * kShardLen;
  auto alloc = member_clients_[new_mi].AsyncAllocateBlocks(MemberQuery(), region);
  CLIO_CO_AWAIT(alloc);
  if (alloc->return_code_ != 0 || alloc->blocks_.size() == 0) {
    HLOG(kError, "safe_bdev AddBdev: AllocateBlocks failed for '{}'",
         task->pool_name_.str());
    member_clients_.pop_back();
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  MemberSlot slot;
  slot.pool_id_ = task->member_pool_id_;
  slot.pool_name_ = task->pool_name_.str();
  slot.node_id_ = task->node_id_;
  slot.state_ = ec::EcState::kActive;
  slot.base_offset_ = alloc->blocks_[0].offset_;

  if (as_parity && current_parity_level_ < max_failures_) {
    // Append parity row r and raise the target level. Parity computation is
    // deferred: re-dirtying every written stripe lets BuildParity compute the
    // new row off the critical path. The incremental RS property means each
    // parity row is an independent function of the data, so existing parity is
    // never read or rewritten.
    const int r = static_cast<int>(current_parity_level_);
    slot.role_ = ec::EcRole::kParity;
    slot.index_ = r;
    members_.push_back(slot);
    ++current_parity_level_;
    {
      std::lock_guard<std::mutex> g(stripe_mu_);
      for (chi::u64 s : written_stripes_) {
        dirty_stripes_.insert(s);
      }
    }
    HLOG(kInfo,
         "safe_bdev AddBdev: parity drive added, parity_level={} "
         "(parity build deferred to BuildParity)",
         current_parity_level_);
  } else {
    // Added as a data member (or parity cap reached). This begins a new column
    // for FUTURE stripes; existing stripes keep their geometry (Part C). For
    // the first-cut milestone we simply register it as a spare-data member.
    slot.role_ = ec::EcRole::kData;
    slot.index_ = -1;  // Not part of the current generation's k columns.
    members_.push_back(slot);
    HLOG(kInfo, "safe_bdev AddBdev: data member registered (generation TODO)");
  }

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::RemoveBdev(ctp::ipc::FullPtr<RemoveBdevTask> task,
                                    chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  (void)rctx;
  for (size_t i = 0; i < members_.size(); ++i) {
    if (members_[i].pool_id_ == task->target_pool_id_) {
      if (task->was_faulty_ != 0) {
        // Mark faulty: excluded from I/O, kept as a recovery candidate.
        members_[i].state_ = ec::EcState::kFaulty;
        HLOG(kInfo, "safe_bdev RemoveBdev: member {} marked faulty", i);
      } else {
        // Clean removal: unlink the member (no migration, no recovery).
        members_.erase(members_.begin() + static_cast<long>(i));
        if (i < member_clients_.size()) {
          member_clients_.erase(member_clients_.begin() +
                                static_cast<long>(i));
        }
        HLOG(kInfo, "safe_bdev RemoveBdev: member {} unlinked", i);
      }
      break;
    }
  }
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::RecoverBdev(ctp::ipc::FullPtr<RecoverBdevTask> task,
                                     chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN

  // Locate the failed member by its (old) pool id.
  int failed = -1;
  for (size_t i = 0; i < members_.size(); ++i) {
    if (members_[i].pool_id_ == task->old_bdev_id_) {
      failed = static_cast<int>(i);
      break;
    }
  }
  if (failed < 0) {
    HLOG(kError, "safe_bdev RecoverBdev: old member {} not found",
         task->old_bdev_id_.ToU64());
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Seat the fresh member: new client, reserve its backing region.
  clio::run::bdev::Client new_client(task->new_pool_id_);
  const chi::u64 region = num_stripes_ * kShardLen;
  auto alloc = new_client.AsyncAllocateBlocks(MemberQuery(), region);
  CLIO_CO_AWAIT(alloc);
  if (alloc->return_code_ != 0 || alloc->blocks_.size() == 0) {
    HLOG(kError, "safe_bdev RecoverBdev: AllocateBlocks failed for '{}'",
         task->pool_name_.str());
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  const chi::u64 new_base = alloc->blocks_[0].offset_;
  member_clients_[static_cast<size_t>(failed)] = new_client;

  auto *ipc = CLIO_IPC;
  const ec::EcRole role = members_[static_cast<size_t>(failed)].role_;
  const int index = members_[static_cast<size_t>(failed)].index_;

  for (chi::u64 s = 0; s < num_stripes_; ++s) {
    // Recovering a DATA member reconstructs its shard from the survivors, which
    // needs current parity. A dirty stripe is unprotected, so refuse rather
    // than write back corrupt data. (Recovering a PARITY member only reads the
    // data shards and re-encodes, so it is unaffected.)
    if (role == ec::EcRole::kData && IsStripeDirty(s)) {
      HLOG(kError,
           "safe_bdev RecoverBdev: stripe {} dirty (parity not built); cannot "
           "reconstruct failed data member",
           s);
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    // Reconstruct the stripe's data from the OTHER survivors.
    std::vector<std::vector<uint8_t>> data_shards;
    bool rd_ok = false;
    CLIO_CO_AWAIT(ReconstructStripe(s, /*exclude=*/failed, rctx, data_shards,
                                    rd_ok));
    if (!rd_ok) {
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }

    ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kShardLen);
    if (buf.IsNull()) {
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    if (role == ec::EcRole::kData) {
      std::memcpy(buf.ptr_, data_shards[static_cast<size_t>(index)].data(),
                  kShardLen);
    } else {
      std::vector<const uint8_t *> ptrs(static_cast<size_t>(k_));
      for (int c = 0; c < k_; ++c) {
        ptrs[static_cast<size_t>(c)] =
            data_shards[static_cast<size_t>(c)].data();
      }
      rs_->EncodeParityShard(index, ptrs, kShardLen,
                             reinterpret_cast<uint8_t *>(buf.ptr_));
    }

    // The member slot's base offset must reflect the new member region during
    // this write.
    members_[static_cast<size_t>(failed)].base_offset_ = new_base;
    auto fut = member_clients_[static_cast<size_t>(failed)].AsyncWrite(
        MemberQuery(), ShardBlocks(members_[static_cast<size_t>(failed)], s),
        buf.shm_.template Cast<void>(), kShardLen);
    CLIO_CO_AWAIT(fut);
    const bool ok = (fut->return_code_ == 0) &&
                    (fut->bytes_written_ == kShardLen);
    ipc->FreeBuffer(buf);
    if (!ok) {
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
  }

  // Bring the member back online on the new pool.
  members_[static_cast<size_t>(failed)].pool_id_ = task->new_pool_id_;
  members_[static_cast<size_t>(failed)].pool_name_ = task->pool_name_.str();
  members_[static_cast<size_t>(failed)].node_id_ = task->node_id_;
  members_[static_cast<size_t>(failed)].base_offset_ = new_base;
  members_[static_cast<size_t>(failed)].state_ = ec::EcState::kActive;

  HLOG(kInfo, "safe_bdev RecoverBdev: member {} recovered onto '{}'", failed,
       task->pool_name_.str());
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::BuildParity(ctp::ipc::FullPtr<BuildParityTask> task,
                                     chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  if (rs_ == nullptr || current_parity_level_ == 0) {
    // No parity configured: nothing to protect. Drop any pending dirty marks.
    std::lock_guard<std::mutex> g(stripe_mu_);
    dirty_stripes_.clear();
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }

  // Snapshot a batch of dirty stripes (max_batch_==0 drains all). Stripes are
  // erased only AFTER their parity is durably written, so an observer that sees
  // an empty dirty set knows every stripe is protected (single builder: this is
  // a periodic task, rescheduled only after completion, so no two BuildParity
  // instances run at once). A stripe that cannot be built (a data member is
  // down) is simply left dirty for a later pass.
  std::vector<chi::u64> batch;
  {
    std::lock_guard<std::mutex> g(stripe_mu_);
    for (chi::u64 s : dirty_stripes_) {
      batch.push_back(s);
      if (task->max_batch_ != 0 &&
          batch.size() >= static_cast<size_t>(task->max_batch_)) {
        break;
      }
    }
  }

  auto *ipc = CLIO_IPC;
  chi::u32 built = 0;
  for (chi::u64 s : batch) {
    // Read the k data shards (all data members must be active to build).
    std::vector<std::vector<uint8_t>> data_shards(
        static_cast<size_t>(k_), std::vector<uint8_t>(kShardLen, 0));
    bool rd_ok = true;
    for (int c = 0; c < k_; ++c) {
      if (members_[static_cast<size_t>(c)].state_ != ec::EcState::kActive) {
        rd_ok = false;
        break;
      }
      bool one = false;
      CLIO_CO_AWAIT(ReadShard(static_cast<size_t>(c), s,
                              data_shards[static_cast<size_t>(c)].data(), rctx,
                              one));
      rd_ok = one;
      if (!rd_ok) {
        break;
      }
    }
    if (!rd_ok) {
      continue;  // leave dirty; retry next pass
    }

    std::vector<const uint8_t *> ptrs(static_cast<size_t>(k_));
    for (int c = 0; c < k_; ++c) {
      ptrs[static_cast<size_t>(c)] = data_shards[static_cast<size_t>(c)].data();
    }

    // (Re)compute and write every active parity row for this stripe.
    bool wr_ok = true;
    for (size_t mi = 0; mi < members_.size(); ++mi) {
      MemberSlot &m = members_[mi];
      if (m.role_ != ec::EcRole::kParity || m.state_ != ec::EcState::kActive) {
        continue;
      }
      ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kShardLen);
      if (buf.IsNull()) {
        wr_ok = false;
        break;
      }
      rs_->EncodeParityShard(m.index_, ptrs, kShardLen,
                             reinterpret_cast<uint8_t *>(buf.ptr_));
      auto fut = member_clients_[mi].AsyncWrite(
          MemberQuery(), ShardBlocks(m, s), buf.shm_.template Cast<void>(),
          kShardLen);
      CLIO_CO_AWAIT(fut);
      wr_ok = (fut->return_code_ == 0) && (fut->bytes_written_ == kShardLen);
      ipc->FreeBuffer(buf);
      if (!wr_ok) {
        break;
      }
    }
    if (!wr_ok) {
      continue;  // leave dirty; retry next pass
    }
    {
      std::lock_guard<std::mutex> g(stripe_mu_);
      dirty_stripes_.erase(s);
    }
    ++built;
  }
  if (built > 0) {
    HLOG(kDebug, "safe_bdev BuildParity: built parity for {} stripe(s)", built);
  }
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::Monitor(ctp::ipc::FullPtr<MonitorTask> task,
                                 chi::RunContext &rctx) {
  CLIO_TASK_BODY_BEGIN
  (void)rctx;
  if (task->query_ == "stats") {
    chi::u32 dirty = 0;
    {
      std::lock_guard<std::mutex> g(stripe_mu_);
      dirty = static_cast<chi::u32>(dirty_stripes_.size());
    }
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(5);
    pk.pack("pool_name");    pk.pack(pool_name_);
    pk.pack("max_failures"); pk.pack(max_failures_);
    pk.pack("num_members");  pk.pack(static_cast<chi::u32>(members_.size()));
    pk.pack("parity_level"); pk.pack(current_parity_level_);
    pk.pack("dirty_stripes"); pk.pack(dirty);
    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  }
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::Destroy(ctp::ipc::FullPtr<DestroyTask> task,
                                 chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  (void)rctx;
  // Member clients/state are released by their destructors.
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::u64 Runtime::GetWorkRemaining() const { return 0; }

}  // namespace clio::run::safe_bdev

// Define ChiMod entry points using CLIO_TASK_CC macro
CLIO_TASK_CC(clio::run::safe_bdev::Runtime)

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
  const int e = EpochOf(stripe);
  if (e < 0) {
    CLIO_CO_RETURN;
  }
  // Snapshot the generation's geometry up front so it stays valid across the
  // co_awaits below (a concurrent AddBdev could reallocate generations_).
  const int gk = generations_[static_cast<size_t>(e)].k;
  const chi::u64 gseed = generations_[static_cast<size_t>(e)].seed;
  const chi::u64 gfirst = generations_[static_cast<size_t>(e)].first_stripe;
  std::vector<int> gen_members = generations_[static_cast<size_t>(e)].member_pos;
  ec::ReedSolomon *grs = generations_[static_cast<size_t>(e)].rs.get();
  const int n = static_cast<int>(gen_members.size());
  const chi::u64 local = stripe - gfirst;
  const int rot = static_cast<int>((local + gseed) % static_cast<chi::u64>(n));
  // RS shard index for member position p (rotation; mirrors ShardOfPosition).
  auto shard_of_pos = [&](int p) { return ((p - rot) % n + n) % n; };

  // Gather up to gen.k active survivors (data + parity) from this generation's
  // member set, excluding `exclude`. Each survivor's global RS shard index
  // comes from the rotated placement for THIS stripe.
  std::vector<int> survivor_index;
  std::vector<std::vector<uint8_t>> survivor_buf;
  for (int p = 0; p < n; ++p) {
    const int mid = gen_members[static_cast<size_t>(p)];
    if (mid == exclude) {
      continue;
    }
    if (members_[static_cast<size_t>(mid)].state_ != ec::EcState::kActive) {
      continue;
    }
    std::vector<uint8_t> buf(kShardLen, 0);
    bool rd_ok = false;
    CLIO_CO_AWAIT(ReadShard(static_cast<size_t>(mid), stripe, buf.data(), rctx,
                            rd_ok));
    if (!rd_ok) {
      CLIO_CO_RETURN;
    }
    survivor_index.push_back(shard_of_pos(p));
    survivor_buf.push_back(std::move(buf));
    if (static_cast<int>(survivor_index.size()) == gk) {
      break;
    }
  }
  if (static_cast<int>(survivor_index.size()) < gk) {
    CLIO_CO_RETURN;  // Too many failures to reconstruct.
  }
  std::vector<const uint8_t *> ptrs(survivor_buf.size());
  for (size_t i = 0; i < survivor_buf.size(); ++i) {
    ptrs[i] = survivor_buf[i].data();
  }
  ok = grs->DecodeData(survivor_index, ptrs, kShardLen, &out);
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
  generations_.clear();
  next_stripe_ = 0;
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
  // column. Slot == global stripe index, so we reserve the FULL stripe capacity
  // up front (region = num_stripes_*kShardLen) on every member.
  const chi::u64 region = num_stripes_ * kShardLen;
  int col = 0;
  std::vector<int> gen0_members;
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
    gen0_members.push_back(col);
    ++col;
  }

  // Open generation 0 over the k initial DATA members with m=0 parity (parity
  // members are added later via AddBdev). The generation owns the entire stripe
  // capacity until a data widening (AddBdev-as-data) closes it. ReedSolomon is
  // built (k, max_failures_) so all parity rows are available as m grows.
  {
    Generation g;
    g.k = k_;
    g.m = 0;
    g.seed = 0;
    g.first_stripe = 0;
    g.num_stripes = 0;  // open
    g.logical_base = 0;
    g.member_pos = gen0_members;
    g.rs = std::make_unique<ec::ReedSolomon>(k_, static_cast<int>(max_failures_));
    generations_.push_back(std::move(g));
  }

  logical_capacity_ =
      static_cast<chi::u64>(k_) * kShardLen * num_stripes_;

  HLOG(kInfo,
       "safe_bdev Create: pool='{}', k={}, M={}, num_stripes={}, "
       "logical_capacity={}, generations=1",
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
  // Bump-allocate whole stripes from the CURRENT (open) generation. Each
  // generation has its own per-stripe data size (gen.k*kShardLen), so the
  // logical space is segmented: allocations advance both the logical cursor and
  // the GLOBAL stripe counter, keeping a clean logical<->stripe mapping.
  if (generations_.empty()) {
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  const Generation &open = generations_.back();
  const chi::u64 per_stripe = static_cast<chi::u64>(open.k) * kShardLen;
  const chi::u64 size = task->size_;
  if (size == 0 || size % per_stripe != 0) {
    HLOG(kError,
         "safe_bdev AllocateBlocks: size {} must be a multiple of the current "
         "generation's stripe size {} (k={})",
         size, per_stripe, open.k);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  const chi::u64 n_stripes = size / per_stripe;
  if (next_stripe_ + n_stripes > num_stripes_) {
    HLOG(kError,
         "safe_bdev AllocateBlocks: out of stripe capacity ({} + {} > {})",
         next_stripe_, n_stripes, num_stripes_);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  const chi::u64 offset = logical_next_;
  logical_next_ += size;
  next_stripe_ += n_stripes;
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
  if (generations_.empty() || task->blocks_.size() == 0) {
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> data = ipc->ToFullPtr(task->data_).template Cast<char>();
  const chi::u64 logical_base = task->blocks_[0].offset_;
  const chi::u64 len = task->length_;

  // Resolve the starting generation + global stripe from the logical offset.
  int epoch = -1;
  chi::u64 g0 = 0;
  int col0 = 0;
  if (!LogicalToStripe(logical_base, &epoch, &g0, &col0) || col0 != 0) {
    HLOG(kError,
         "safe_bdev Write: offset {} is not stripe-aligned / out of range",
         logical_base);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  // This first cut assumes full-stripe writes that do not cross a generation
  // boundary (the tests write one generation's stripes at a time). The stripe
  // data size is fixed by the generation's k.
  const int gk = generations_[static_cast<size_t>(epoch)].k;
  const chi::u64 per_stripe_data = static_cast<chi::u64>(gk) * kShardLen;
  if (len % per_stripe_data != 0) {
    HLOG(kError,
         "safe_bdev Write: unaligned write (offset={}, len={}, stripe={}) "
         "not supported in first-cut milestone",
         logical_base, len, per_stripe_data);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  const chi::u64 n_stripes = len / per_stripe_data;
  chi::u64 bytes_written = 0;

  for (chi::u64 si = 0; si < n_stripes; ++si) {
    const chi::u64 s = g0 + si;
    const int e = EpochOf(s);
    if (e < 0 || generations_[static_cast<size_t>(e)].k != gk) {
      // Crossed into another generation (different width): unsupported here.
      HLOG(kError, "safe_bdev Write: stripe {} crosses a generation boundary",
           s);
      task->bytes_written_ = bytes_written;
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    const Generation &gen = generations_[static_cast<size_t>(e)];

    // Slice the gen.k data shards for this stripe out of the host buffer and
    // write each to its rotated member (declustered placement).
    std::vector<chi::Future<WriteTask>> futs;
    std::vector<ctp::ipc::FullPtr<char>> bufs;
    bool dispatch_ok = true;
    for (int c = 0; c < gk; ++c) {
      const int mid = MemberOfShard(gen, s, c);
      MemberSlot &m = members_[static_cast<size_t>(mid)];
      if (m.state_ != ec::EcState::kActive) {
        continue;
      }
      ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kShardLen);
      if (buf.IsNull()) {
        dispatch_ok = false;
        break;
      }
      const chi::u64 off =
          si * per_stripe_data + static_cast<chi::u64>(c) * kShardLen;
      std::memcpy(buf.ptr_, data.ptr_ + off, kShardLen);
      bufs.push_back(buf);
      futs.push_back(member_clients_[static_cast<size_t>(mid)].AsyncWrite(
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
  if (generations_.empty() || task->blocks_.size() == 0) {
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> data = ipc->ToFullPtr(task->data_).template Cast<char>();
  const chi::u64 logical_base = task->blocks_[0].offset_;
  const chi::u64 len = task->length_;

  int epoch = -1;
  chi::u64 g0 = 0;
  int col0 = 0;
  if (!LogicalToStripe(logical_base, &epoch, &g0, &col0) || col0 != 0) {
    HLOG(kError,
         "safe_bdev Read: offset {} is not stripe-aligned / out of range",
         logical_base);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  const int gk = generations_[static_cast<size_t>(epoch)].k;
  const chi::u64 per_stripe_data = static_cast<chi::u64>(gk) * kShardLen;
  if (len % per_stripe_data != 0) {
    HLOG(kError,
         "safe_bdev Read: unaligned read (offset={}, len={}, stripe={}) "
         "not supported in first-cut milestone",
         logical_base, len, per_stripe_data);
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  const chi::u64 n_stripes = len / per_stripe_data;
  chi::u64 bytes_read = 0;

  for (chi::u64 si = 0; si < n_stripes; ++si) {
    const chi::u64 s = g0 + si;
    const int e = EpochOf(s);
    if (e < 0 || generations_[static_cast<size_t>(e)].k != gk) {
      HLOG(kError, "safe_bdev Read: stripe {} crosses a generation boundary",
           s);
      task->bytes_read_ = bytes_read;
      task->length_ = bytes_read;
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    const Generation &gen = generations_[static_cast<size_t>(e)];

    // Are all data members for this stripe active? (Their identity rotates per
    // stripe.) If so, plain parallel read of the rotated data shards; else
    // reconstruct.
    bool all_data_active = true;
    for (int c = 0; c < gk; ++c) {
      const int mid = MemberOfShard(gen, s, c);
      if (members_[static_cast<size_t>(mid)].state_ != ec::EcState::kActive) {
        all_data_active = false;
        break;
      }
    }

    std::vector<std::vector<uint8_t>> data_shards;
    bool stripe_ok = true;
    if (all_data_active) {
      data_shards.assign(static_cast<size_t>(gk),
                         std::vector<uint8_t>(kShardLen, 0));
      for (int c = 0; c < gk && stripe_ok; ++c) {
        const int mid = MemberOfShard(gen, s, c);
        bool rd_ok = false;
        CLIO_CO_AWAIT(ReadShard(static_cast<size_t>(mid), s,
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

    // Copy the gen.k data shards out into the caller's buffer.
    for (int c = 0; c < gk; ++c) {
      const chi::u64 off =
          si * per_stripe_data + static_cast<chi::u64>(c) * kShardLen;
      std::memcpy(data.ptr_ + off, data_shards[static_cast<size_t>(c)].data(),
                  kShardLen);
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
    // Append a parity member to the CURRENT (open) generation and raise its m
    // (and the global parity level). Parity computation is deferred: re-dirty
    // this generation's written stripes so BuildParity computes the new row off
    // the critical path. The incremental RS property means each parity row is
    // an independent function of the data, so existing parity is never read or
    // rewritten.
    const int r = static_cast<int>(current_parity_level_);
    slot.role_ = ec::EcRole::kParity;
    slot.index_ = r;
    const int new_mid = static_cast<int>(members_.size());
    members_.push_back(slot);
    Generation &open = generations_.back();
    open.member_pos.push_back(new_mid);  // N = k+m grows by one
    ++open.m;
    ++current_parity_level_;
    {
      std::lock_guard<std::mutex> g(stripe_mu_);
      for (chi::u64 s : written_stripes_) {
        if (EpochOf(s) == static_cast<int>(generations_.size()) - 1) {
          dirty_stripes_.insert(s);
        }
      }
    }
    HLOG(kInfo,
         "safe_bdev AddBdev: parity drive added to generation {}, "
         "parity_level={} (parity build deferred to BuildParity)",
         generations_.size() - 1, current_parity_level_);
  } else {
    // Added as a DATA member: this is the variable-width widening. CLOSE the
    // current generation (freeze its stripe span) and OPEN a new generation
    // with k+1 data columns over {previous data members, new data member,
    // previous parity members}. Future writes widen; existing stripes keep
    // their geometry (Part C) — no data is rewritten.
    slot.role_ = ec::EcRole::kData;
    const int new_mid = static_cast<int>(members_.size());
    slot.index_ = -1;  // legacy hint only; placement is rotation-based
    members_.push_back(slot);

    Generation &cur = generations_.back();
    // Freeze the closing generation's span at what has been allocated so far.
    cur.num_stripes = next_stripe_ - cur.first_stripe;

    // Build the new generation's member list: data members of the closing gen
    // (positions [0,k)) + the new data member, then the parity members
    // (positions [k, k+m)).
    std::vector<int> new_members;
    new_members.reserve(cur.member_pos.size() + 1);
    for (int c = 0; c < cur.k; ++c) {
      new_members.push_back(cur.member_pos[static_cast<size_t>(c)]);
    }
    new_members.push_back(new_mid);
    for (int pr = 0; pr < cur.m; ++pr) {
      new_members.push_back(cur.member_pos[static_cast<size_t>(cur.k + pr)]);
    }

    Generation ng;
    ng.k = cur.k + 1;
    ng.m = cur.m;
    ng.seed = static_cast<chi::u64>(generations_.size());
    ng.first_stripe = next_stripe_;
    ng.num_stripes = 0;  // open
    ng.logical_base = logical_next_;
    ng.member_pos = std::move(new_members);
    ng.rs = std::make_unique<ec::ReedSolomon>(ng.k,
                                              static_cast<int>(max_failures_));
    k_ = ng.k;
    generations_.push_back(std::move(ng));

    // Recompute usable logical capacity: bytes already committed to closed
    // generations plus the open generation's remaining stripe budget.
    chi::u64 closed_bytes = logical_next_;
    chi::u64 remaining_stripes = num_stripes_ - next_stripe_;
    logical_capacity_ =
        closed_bytes +
        static_cast<chi::u64>(k_) * kShardLen * remaining_stripes;

    HLOG(kInfo,
         "safe_bdev AddBdev: data member added — opened generation {} (k={}, "
         "m={}, first_stripe={})",
         generations_.size() - 1, k_, generations_.back().m,
         generations_.back().first_stripe);
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
        // Clean removal: mark the member removed (no migration, no recovery).
        // We do NOT erase it from members_/member_clients_ because the global
        // member id is referenced by every generation's member_pos; erasing
        // would shift indices and corrupt placement. kRemoved is excluded from
        // I/O exactly like kFaulty but is not a recovery candidate.
        members_[i].state_ = ec::EcState::kRemoved;
        HLOG(kInfo, "safe_bdev RemoveBdev: member {} unlinked (marked removed)",
             i);
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
  // Point the slot at the new backing region now; the failed member is still
  // excluded from reconstruction via `exclude=failed`, and stays non-active
  // until the whole recovery completes.
  members_[static_cast<size_t>(failed)].base_offset_ = new_base;

  auto *ipc = CLIO_IPC;

  // Reconstruct the failed member's shards across EVERY generation it belongs
  // to (mirrors DeclusteredArray::RecoverMember). The role it plays for each
  // stripe (data column vs. parity row) is placement-derived, not fixed.
  for (size_t ei = 0; ei < generations_.size(); ++ei) {
    // Snapshot this generation's geometry (stable across co_awaits).
    const int gk = generations_[ei].k;
    const chi::u64 gseed = generations_[ei].seed;
    const chi::u64 gfirst = generations_[ei].first_stripe;
    const chi::u64 gspan = (generations_[ei].num_stripes != 0)
                               ? generations_[ei].num_stripes
                               : (next_stripe_ - gfirst);
    std::vector<int> gmembers = generations_[ei].member_pos;
    ec::ReedSolomon *grs = generations_[ei].rs.get();
    const int n = static_cast<int>(gmembers.size());
    int pos = -1;
    for (int p = 0; p < n; ++p) {
      if (gmembers[static_cast<size_t>(p)] == failed) {
        pos = p;
        break;
      }
    }
    if (pos < 0) {
      continue;  // member not in this generation
    }

    for (chi::u64 s = gfirst; s < gfirst + gspan; ++s) {
      const chi::u64 local = s - gfirst;
      const int rot =
          static_cast<int>((local + gseed) % static_cast<chi::u64>(n));
      const int q = ((pos - rot) % n + n) % n;  // RS shard for this stripe

      // Recovering a DATA shard reconstructs from survivors, which needs
      // current parity. A dirty stripe is unprotected, so refuse rather than
      // write back corrupt data. (A parity shard only reads data + re-encodes,
      // so it is unaffected.)
      if (q < gk && IsStripeDirty(s)) {
        HLOG(kError,
             "safe_bdev RecoverBdev: stripe {} dirty (parity not built); "
             "cannot reconstruct failed data shard",
             s);
        task->return_code_ = 1;
        CLIO_CO_RETURN;
      }

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
      if (q < gk) {
        std::memcpy(buf.ptr_, data_shards[static_cast<size_t>(q)].data(),
                    kShardLen);
      } else {
        std::vector<const uint8_t *> ptrs(static_cast<size_t>(gk));
        for (int c = 0; c < gk; ++c) {
          ptrs[static_cast<size_t>(c)] =
              data_shards[static_cast<size_t>(c)].data();
        }
        grs->EncodeParityShard(q - gk, ptrs, kShardLen,
                               reinterpret_cast<uint8_t *>(buf.ptr_));
      }

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
  if (current_parity_level_ == 0) {
    // No parity configured anywhere: nothing to protect. Drop pending marks.
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
    const int e = EpochOf(s);
    if (e < 0) {
      continue;
    }
    // Snapshot this generation's geometry (stable across co_awaits below).
    const int gk = generations_[static_cast<size_t>(e)].k;
    const int gm = generations_[static_cast<size_t>(e)].m;
    if (gm == 0) {
      // This generation has no parity members yet: nothing to build. Clear the
      // mark so an observer sees the stripe as "as protected as it can be".
      std::lock_guard<std::mutex> g(stripe_mu_);
      dirty_stripes_.erase(s);
      continue;
    }
    ec::ReedSolomon *grs = generations_[static_cast<size_t>(e)].rs.get();

    // Read the gen.k data shards from their rotated members (all must be
    // active to build parity).
    std::vector<std::vector<uint8_t>> data_shards(
        static_cast<size_t>(gk), std::vector<uint8_t>(kShardLen, 0));
    bool rd_ok = true;
    for (int c = 0; c < gk; ++c) {
      const int mid = MemberOfShard(generations_[static_cast<size_t>(e)], s, c);
      if (members_[static_cast<size_t>(mid)].state_ != ec::EcState::kActive) {
        rd_ok = false;
        break;
      }
      bool one = false;
      CLIO_CO_AWAIT(ReadShard(static_cast<size_t>(mid), s,
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

    std::vector<const uint8_t *> ptrs(static_cast<size_t>(gk));
    for (int c = 0; c < gk; ++c) {
      ptrs[static_cast<size_t>(c)] = data_shards[static_cast<size_t>(c)].data();
    }

    // (Re)compute and write every active parity row for this stripe onto its
    // rotated parity member.
    bool wr_ok = true;
    for (int pr = 0; pr < gm; ++pr) {
      const int mid =
          MemberOfShard(generations_[static_cast<size_t>(e)], s, gk + pr);
      MemberSlot &m = members_[static_cast<size_t>(mid)];
      if (m.state_ != ec::EcState::kActive) {
        continue;
      }
      ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kShardLen);
      if (buf.IsNull()) {
        wr_ok = false;
        break;
      }
      grs->EncodeParityShard(pr, ptrs, kShardLen,
                             reinterpret_cast<uint8_t *>(buf.ptr_));
      auto fut = member_clients_[static_cast<size_t>(mid)].AsyncWrite(
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
    pk.pack_map(6);
    pk.pack("pool_name");    pk.pack(pool_name_);
    pk.pack("max_failures"); pk.pack(max_failures_);
    pk.pack("num_members");  pk.pack(static_cast<chi::u32>(members_.size()));
    pk.pack("parity_level"); pk.pack(current_parity_level_);
    pk.pack("dirty_stripes"); pk.pack(dirty);
    pk.pack("num_generations");
    pk.pack(static_cast<chi::u32>(generations_.size()));
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

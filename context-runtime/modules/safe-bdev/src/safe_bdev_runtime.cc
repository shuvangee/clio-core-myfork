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

#include <algorithm>
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

chi::priv::vector<clio::run::bdev::Block> Runtime::MemberBlocks(
    chi::u64 offset, chi::u64 len) const {
  chi::priv::vector<clio::run::bdev::Block> blocks(CTP_MALLOC);
  blocks.push_back(clio::run::bdev::Block(offset, len, 0));
  return blocks;
}

//===========================================================================
// Segment I/O helpers (run inside task fibers; co_await member bdev I/O)
//===========================================================================

chi::TaskResume Runtime::WriteSegment(size_t mi, chi::u64 offset,
                                      const uint8_t *src, chi::u64 len,
                                      chi::RunContext &ctx, bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(len);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  std::memcpy(buf.ptr_, src, len);
  auto fut = member_clients_[mi].AsyncWrite(
      MemberQuery(), MemberBlocks(offset, len),
      buf.shm_.template Cast<void>(), len);
  CLIO_CO_AWAIT(fut);
  ok = (fut->return_code_ == 0) && (fut->bytes_written_ == len);
  MaybeFaultMember(mi, fut->io_error_);
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::ReadSegment(size_t mi, chi::u64 offset, uint8_t *dst,
                                     chi::u64 len, chi::RunContext &ctx,
                                     bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(len);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  auto fut = member_clients_[mi].AsyncRead(
      MemberQuery(), MemberBlocks(offset, len),
      buf.shm_.template Cast<void>(), len);
  CLIO_CO_AWAIT(fut);
  if (fut->return_code_ == 0 && fut->bytes_read_ == len) {
    std::memcpy(dst, buf.ptr_, len);
    ok = true;
  }
  MaybeFaultMember(mi, fut->io_error_);
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

//===========================================================================
// Member superblock helpers
//===========================================================================

chi::TaskResume Runtime::WriteSuperblock(size_t mi, chi::RunContext &ctx,
                                         bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kSuperblockSize);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  // Serialize the array identity into a zero-padded superblock buffer.
  std::memset(buf.ptr_, 0, kSuperblockSize);
  MemberSuperblock sb;
  sb.magic = kMemberSuperblockMagic;
  sb.format_version = kMemberSuperblockVersion;
  sb.flags = 0;
  sb.array_major = static_cast<uint64_t>(pool_id_.major_);
  sb.array_minor = static_cast<uint64_t>(pool_id_.minor_);
  sb.member_slot = static_cast<uint32_t>(mi);
  sb.role = static_cast<uint32_t>(members_[mi].role_);
  sb.index = static_cast<uint32_t>(members_[mi].index_);
  sb.max_failures = max_failures_;
  sb.shard_len = kChunkLen;
  sb.epoch = 0;
  sb.checksum = sb.ComputeChecksum();
  std::memcpy(buf.ptr_, &sb, sizeof(sb));

  // The superblock lives at absolute offset 0 on the member.
  auto fut = member_clients_[mi].AsyncWrite(
      MemberQuery(), MemberBlocks(0, kSuperblockSize),
      buf.shm_.template Cast<void>(), kSuperblockSize);
  CLIO_CO_AWAIT(fut);
  ok = (fut->return_code_ == 0) && (fut->bytes_written_ == kSuperblockSize);
  MaybeFaultMember(mi, fut->io_error_);
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::ReadSuperblock(size_t mi, chi::RunContext &ctx,
                                        MemberSuperblock &sb, bool &present,
                                        bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  present = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kSuperblockSize);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  std::memset(buf.ptr_, 0, kSuperblockSize);
  auto fut = member_clients_[mi].AsyncRead(
      MemberQuery(), MemberBlocks(0, kSuperblockSize),
      buf.shm_.template Cast<void>(), kSuperblockSize);
  CLIO_CO_AWAIT(fut);
  if (fut->return_code_ == 0 && fut->bytes_read_ == kSuperblockSize) {
    ok = true;
    std::memcpy(&sb, buf.ptr_, sizeof(sb));
    present = sb.Validate();
  }
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

//===========================================================================
// Row reconstruction (decode the k data chunks of a row from survivors)
//===========================================================================

chi::TaskResume Runtime::ReconstructRow(
    chi::u64 row, int exclude, chi::RunContext &ctx,
    std::vector<std::vector<uint8_t>> &out, bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  const chi::u64 off = ChunkOffset(row);

  // Gather up to k active survivors among the k data + m parity members at
  // this row offset, excluding `exclude` and any non-active member. Each
  // survivor's global RS shard index is its data column i (data) or k+j
  // (parity j) — fixed roles, no rotation.
  std::vector<int> survivor_index;
  std::vector<std::vector<uint8_t>> survivor_buf;
  const int total = static_cast<int>(members_.size());
  for (int mid = 0; mid < total; ++mid) {
    if (mid == exclude) {
      continue;
    }
    const MemberSlot &m = members_[static_cast<size_t>(mid)];
    if (m.state_ != ec::EcState::kActive) {
      continue;
    }
    std::vector<uint8_t> buf(kChunkLen, 0);
    bool rd_ok = false;
    CLIO_CO_AWAIT(ReadSegment(static_cast<size_t>(mid), off, buf.data(),
                              kChunkLen, rctx, rd_ok));
    if (!rd_ok) {
      CLIO_CO_RETURN;
    }
    const int global_shard =
        (m.role_ == ec::EcRole::kData) ? m.index_ : (k_ + m.index_);
    survivor_index.push_back(global_shard);
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
  ok = rs_->DecodeData(survivor_index, ptrs, kChunkLen, &out);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::ReconstructDataChunk(chi::u64 row, int data_col,
                                              int exclude, chi::RunContext &ctx,
                                              std::vector<uint8_t> &out,
                                              bool &ok) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  ok = false;
  std::vector<std::vector<uint8_t>> data_chunks;
  bool rec_ok = false;
  CLIO_CO_AWAIT(ReconstructRow(row, exclude, rctx, data_chunks, rec_ok));
  if (!rec_ok) {
    CLIO_CO_RETURN;
  }
  out = std::move(data_chunks[static_cast<size_t>(data_col)]);
  ok = true;
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
  parity_level_ = 0;
  members_.clear();
  member_clients_.clear();
  reattached_members_ = 0;
  {
    std::lock_guard<std::mutex> g(row_mu_);
    dirty_rows_.clear();
    written_rows_.clear();
  }

  // Initial members are DATA members; k = number supplied.
  k_ = static_cast<int>(params.members_.size());
  if (k_ <= 0) {
    HLOG(kError, "safe_bdev Create: no member bdevs supplied");
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Query the smallest member's remaining capacity. usable_per_member is the
  // largest multiple of kChunkLen that fits after the superblock; num_rows is
  // that many chunks, and logical_capacity is k*num_rows*kChunkLen.
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
  const chi::u64 avail =
      (min_remaining > kSuperblockSize) ? (min_remaining - kSuperblockSize) : 0;
  const chi::u64 usable_per_member = (avail / kChunkLen) * kChunkLen;
  num_rows_ = usable_per_member / kChunkLen;
  if (num_rows_ == 0) {
    HLOG(kError, "safe_bdev Create: member too small for even one chunk");
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Seat each member as a DATA column. Before seating, read the member's
  // superblock to classify it:
  //   - not present (blank/zeros)  -> FRESH: write our identity.
  //   - present AND it is OURS     -> re-attach: reuse, do not rewrite data.
  //   - present AND it is FOREIGN  -> REFUSE: another array owns it.
  int col = 0;
  for (const auto &desc : params.members_) {
    MemberSlot slot;
    slot.pool_id_ = desc.pool_id_;
    slot.pool_name_ = desc.pool_name_;
    slot.node_id_ = desc.node_id_;
    slot.role_ = ec::EcRole::kData;
    slot.state_ = ec::EcState::kActive;
    slot.index_ = col;
    members_.push_back(slot);

    MemberSuperblock sb;
    bool present = false;
    bool sb_ok = false;
    CLIO_CO_AWAIT(ReadSuperblock(static_cast<size_t>(col), rctx, sb, present,
                                 sb_ok));
    if (!sb_ok) {
      HLOG(kError, "safe_bdev Create: superblock read failed for member '{}'",
           desc.pool_name_);
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    if (!present) {
      bool wr_ok = false;
      CLIO_CO_AWAIT(WriteSuperblock(static_cast<size_t>(col), rctx, wr_ok));
      if (!wr_ok) {
        HLOG(kError,
             "safe_bdev Create: superblock write failed for fresh member '{}'",
             desc.pool_name_);
        task->return_code_ = 1;
        CLIO_CO_RETURN;
      }
      HLOG(kInfo, "safe_bdev Create: initialized fresh member '{}' (slot {})",
           desc.pool_name_, col);
    } else if (sb.array_major == static_cast<uint64_t>(pool_id_.major_) &&
               sb.array_minor == static_cast<uint64_t>(pool_id_.minor_)) {
      ++reattached_members_;
      HLOG(kInfo,
           "safe_bdev Create: re-attached member '{}' (slot {}) already owned "
           "by this array ({},{})",
           desc.pool_name_, col, pool_id_.major_, pool_id_.minor_);
    } else {
      HLOG(kError,
           "safe_bdev Create: REFUSING member '{}' — already initialized by a "
           "FOREIGN array ({},{}); this array is ({},{})",
           desc.pool_name_, sb.array_major, sb.array_minor, pool_id_.major_,
           pool_id_.minor_);
      task->return_code_ = 2;
      CLIO_CO_RETURN;
    }
    ++col;
  }

  // One Reed-Solomon code (k, max_failures). Parity rows become available as
  // parity members are appended via AddBdev.
  rs_ = std::make_unique<ec::ReedSolomon>(k_, static_cast<int>(max_failures_));

  logical_capacity_ = static_cast<chi::u64>(k_) * num_rows_ * kChunkLen;

  // Real reclaimable allocator over the logical capacity. Reuse bdev's
  // free-list (GlobalBlockMap) + bump heap (Heap): AllocateBlocks tries the
  // free list first, then the heap; FreeBlocks returns blocks to the free
  // list. alignment 4096 matches bdev. allocated_bytes_ tracks the live set
  // for GetStats.
  {
    chi::WorkOrchestrator *wo = CLIO_WORK_ORCHESTRATOR;
    const size_t num_workers = (wo != nullptr) ? wo->GetWorkerCount() : 16;
    block_map_.Init(num_workers);
    heap_.Init(logical_capacity_, /*alignment=*/4096);
    allocated_bytes_.store(0);
  }

  HLOG(kInfo,
       "safe_bdev Create: pool='{}', k={}, M={}, num_rows={}, "
       "logical_capacity={} (RAID-0 data + dedicated parity)",
       task->pool_name_.str(), k_, max_failures_, num_rows_, logical_capacity_);

  // Kick off the background parity builder. Write records dirty rows off the
  // critical path; this periodic task converges them to full protection.
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
  // Real reclaimable allocation over the logical space: try the free list
  // first (reuse a freed block), else carve from the heap. This mirrors bdev's
  // allocator so freed blocks are reclaimed and block sizes are uneven.
  const chi::u64 size = task->size_;
  if (size == 0) {
    task->blocks_.clear();
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }
  chi::Worker *worker = CLIO_CUR_WORKER;
  const int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId())
                                            : 0;

  clio::run::bdev::Block block;
  bool allocated = block_map_.AllocateBlock(worker_id, size, block);
  if (!allocated) {
    // Heap snaps the size up to a 4096-aligned block_type bucket (like bdev).
    int block_type = 0;
    const size_t buckets[] = {4096,    16384,   32768,
                              65536,   131072,  1048576};
    for (size_t i = 0; i < 6; ++i) {
      if (buckets[i] >= size) {
        block_type = static_cast<int>(i);
        break;
      }
      block_type = 5;
    }
    allocated = heap_.Allocate(size, block_type, block);
  }
  if (!allocated) {
    HLOG(kError, "safe_bdev AllocateBlocks: out of logical capacity ({} bytes)",
         size);
    task->blocks_.clear();
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  allocated_bytes_.fetch_add(block.size_, std::memory_order_relaxed);
  task->blocks_.clear();
  task->blocks_.push_back(block);
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::FreeBlocks(ctp::ipc::FullPtr<FreeBlocksTask> task,
                                    chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN
  (void)rctx;
  // Return blocks to the free list (real reclaim). Normalize block_type_ from
  // the block SIZE so AllocateBlock (which picks the free list by size class)
  // can find them again — mirrors bdev::FreeBlocks.
  chi::Worker *worker = CLIO_CUR_WORKER;
  const int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId())
                                            : 0;
  const size_t buckets[] = {4096, 16384, 32768, 65536, 131072, 1048576};
  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    clio::run::bdev::Block b = task->blocks_[i];
    int bt = 5;
    for (size_t j = 0; j < 6; ++j) {
      if (buckets[j] >= static_cast<size_t>(b.size_)) {
        bt = static_cast<int>(j);
        break;
      }
    }
    b.block_type_ = static_cast<chi::u32>(bt);
    block_map_.FreeBlock(worker_id, b);
    const chi::u64 cur = allocated_bytes_.load(std::memory_order_relaxed);
    allocated_bytes_.store(cur >= b.size_ ? cur - b.size_ : 0,
                           std::memory_order_relaxed);
  }
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
  ctp::ipc::FullPtr<char> data =
      ipc->ToFullPtr(task->data_).template Cast<char>();

  // The data buffer fills the allocated blocks in order. For each block, walk
  // its CHUNK-segments: a segment is the block's intersection with one RAID-0
  // chunk [ci*kChunkLen, (ci+1)*kChunkLen). It maps to DATA member (ci % k) at
  // physical offset kSuperblockSize + (ci/k)*kChunkLen + (within-chunk start),
  // length = segment size. Segments across data members are dispatched in
  // parallel. Parity is deferred (rows marked dirty).
  chi::u64 buf_pos = 0;  // running offset into the host data buffer
  chi::u64 bytes_written = 0;
  std::set<chi::u64> touched_rows;

  for (size_t bi = 0; bi < task->blocks_.size(); ++bi) {
    const chi::u64 lo = task->blocks_[bi].offset_;
    const chi::u64 ls = task->blocks_[bi].size_;
    const chi::u64 hi = lo + ls;

    std::vector<chi::Future<WriteTask>> futs;
    std::vector<ctp::ipc::FullPtr<char>> bufs;
    bool dispatch_ok = true;

    chi::u64 cur = lo;
    while (cur < hi && dispatch_ok) {
      const chi::u64 ci = cur / kChunkLen;
      const chi::u64 chunk_end = (ci + 1) * kChunkLen;
      const chi::u64 seg_end = std::min(hi, chunk_end);
      const chi::u64 seg_len = seg_end - cur;
      const int data_col = static_cast<int>(ci % static_cast<chi::u64>(k_));
      const chi::u64 row = ci / static_cast<chi::u64>(k_);
      const chi::u64 within = cur % kChunkLen;
      const chi::u64 phys = ChunkOffset(row) + within;

      MemberSlot &m = members_[static_cast<size_t>(data_col)];
      if (m.state_ == ec::EcState::kActive) {
        ctp::ipc::FullPtr<char> seg = ipc->AllocateBuffer(seg_len);
        if (seg.IsNull()) {
          dispatch_ok = false;
          break;
        }
        std::memcpy(seg.ptr_, data.ptr_ + buf_pos, seg_len);
        bufs.push_back(seg);
        futs.push_back(member_clients_[static_cast<size_t>(data_col)].AsyncWrite(
            MemberQuery(), MemberBlocks(phys, seg_len),
            seg.shm_.template Cast<void>(), seg_len));
      }
      touched_rows.insert(row);
      buf_pos += seg_len;
      cur = seg_end;
    }

    bool block_ok = dispatch_ok;
    for (auto &f : futs) {
      CLIO_CO_AWAIT(f);
      if (f->return_code_ != 0) {
        block_ok = false;
      }
    }
    for (auto &b : bufs) {
      ipc->FreeBuffer(b);
    }
    if (!block_ok) {
      task->bytes_written_ = bytes_written;
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    bytes_written += ls;
  }

  // Mark all rows the write touched dirty; BuildParity (re)computes parity.
  for (chi::u64 r : touched_rows) {
    MarkRowDirty(r);
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
  ctp::ipc::FullPtr<char> data =
      ipc->ToFullPtr(task->data_).template Cast<char>();

  // Symmetric decomposition of Write: walk each block's chunk-segments. If the
  // segment's DATA member is active, AsyncRead it directly; if faulty,
  // reconstruct that member's FULL chunk for the row (guarding dirty rows) and
  // copy the needed within-slice.
  chi::u64 buf_pos = 0;
  chi::u64 bytes_read = 0;

  for (size_t bi = 0; bi < task->blocks_.size(); ++bi) {
    const chi::u64 lo = task->blocks_[bi].offset_;
    const chi::u64 ls = task->blocks_[bi].size_;
    const chi::u64 hi = lo + ls;

    chi::u64 cur = lo;
    while (cur < hi) {
      const chi::u64 ci = cur / kChunkLen;
      const chi::u64 chunk_end = (ci + 1) * kChunkLen;
      const chi::u64 seg_end = std::min(hi, chunk_end);
      const chi::u64 seg_len = seg_end - cur;
      const int data_col = static_cast<int>(ci % static_cast<chi::u64>(k_));
      const chi::u64 row = ci / static_cast<chi::u64>(k_);
      const chi::u64 within = cur % kChunkLen;
      const chi::u64 phys = ChunkOffset(row) + within;

      const MemberSlot &m = members_[static_cast<size_t>(data_col)];
      bool seg_ok = false;
      if (m.state_ == ec::EcState::kActive) {
        CLIO_CO_AWAIT(ReadSegment(static_cast<size_t>(data_col), phys,
                                  reinterpret_cast<uint8_t *>(data.ptr_) +
                                      buf_pos,
                                  seg_len, rctx, seg_ok));
      } else if (IsRowDirty(row)) {
        // A data member is down AND this row's parity is not current: it is
        // unprotected, so reconstruction would yield wrong bytes. Fail loudly.
        HLOG(kError,
             "safe_bdev Read: row {} is dirty (parity not built) and data "
             "member {} is down — cannot reconstruct",
             row, data_col);
        seg_ok = false;
      } else {
        std::vector<uint8_t> chunk;
        CLIO_CO_AWAIT(ReconstructDataChunk(row, data_col, /*exclude=*/-1, rctx,
                                           chunk, seg_ok));
        if (seg_ok) {
          std::memcpy(data.ptr_ + buf_pos, chunk.data() + within, seg_len);
        }
      }
      if (!seg_ok) {
        task->bytes_read_ = bytes_read;
        task->length_ = bytes_read;
        task->return_code_ = 1;
        CLIO_CO_RETURN;
      }
      buf_pos += seg_len;
      cur = seg_end;
    }
    bytes_read += ls;
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
  (void)rctx;
  // Usable remaining from the allocator's live set.
  const chi::u64 used = allocated_bytes_.load(std::memory_order_relaxed);
  task->remaining_size_ =
      (logical_capacity_ > used) ? (logical_capacity_ - used) : 0;
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

chi::TaskResume Runtime::AddBdev(ctp::ipc::FullPtr<AddBdevTask> task,
                                 chi::RunContext &ctx) {
  chi::RunContext &rctx = ctx;
  CLIO_TASK_BODY_BEGIN

  const bool as_parity = (task->as_parity_ != 0);

  if (!as_parity) {
    // Adding a DATA member would change the RAID-0 mapping (k changes the
    // column count and chunk placement of every existing logical offset) and
    // requires a reshape. Unsupported in this fixed-k RAID-0 model.
    HLOG(kError,
         "safe_bdev AddBdev: add-data/reshape not supported in RAID-0 mode "
         "(member '{}'); TODO #543",
         task->pool_name_.str());
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  if (parity_level_ >= max_failures_) {
    HLOG(kWarning,
         "safe_bdev AddBdev: parity already at max_failures={} (member '{}' "
         "not added)",
         max_failures_, task->pool_name_.str());
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Append a PARITY member. Its parity row j == parity_level_, global RS shard
  // index = k + j. Parity computation is deferred: re-dirty every written row
  // so BuildParity computes the new parity column off the critical path. The
  // incremental RS property means each parity row is an independent function of
  // the data, so existing parity is never read or rewritten.
  member_clients_.emplace_back(task->member_pool_id_);
  MemberSlot slot;
  slot.pool_id_ = task->member_pool_id_;
  slot.pool_name_ = task->pool_name_.str();
  slot.node_id_ = task->node_id_;
  slot.role_ = ec::EcRole::kParity;
  slot.state_ = ec::EcState::kActive;
  slot.index_ = static_cast<int>(parity_level_);
  const size_t new_mi = members_.size();
  members_.push_back(slot);
  ++parity_level_;

  {
    std::lock_guard<std::mutex> g(row_mu_);
    for (chi::u64 r : written_rows_) {
      dirty_rows_.insert(r);
    }
  }
  HLOG(kInfo,
       "safe_bdev AddBdev: parity drive added (parity_level={}, all written "
       "rows re-dirtied; parity build deferred to BuildParity)",
       parity_level_);

  // Stamp the new member's superblock with this array's identity.
  {
    bool wr_ok = false;
    CLIO_CO_AWAIT(WriteSuperblock(new_mi, rctx, wr_ok));
    if (!wr_ok) {
      HLOG(kWarning,
           "safe_bdev AddBdev: superblock write failed for new parity member "
           "'{}' (member seated; will be re-stamped on next attach)",
           task->pool_name_.str());
    }
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
        // Clean removal: mark removed (excluded from I/O like faulty, but not
        // a recovery candidate). We do NOT erase the slot because data column
        // / parity row index is positional.
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

  // Seat the fresh member on its new pool. It stays non-active and is excluded
  // from reconstruction via `exclude=failed` until recovery completes.
  member_clients_[static_cast<size_t>(failed)] =
      clio::run::bdev::Client(task->new_pool_id_);

  const bool is_data = (members_[static_cast<size_t>(failed)].role_ ==
                        ec::EcRole::kData);
  const int idx = members_[static_cast<size_t>(failed)].index_;
  auto *ipc = CLIO_IPC;

  // Rebuild the failed member's chunk for every row. A DATA member's chunk is
  // reconstructed by decoding the row excluding it (guard dirty rows). A PARITY
  // member's chunk is re-encoded from the row's k data chunks (no dirty guard
  // needed: it only reads data + re-encodes).
  for (chi::u64 r = 0; r < num_rows_; ++r) {
    if (is_data && IsRowDirty(r)) {
      HLOG(kError,
           "safe_bdev RecoverBdev: row {} dirty (parity not built); cannot "
           "reconstruct failed data member",
           r);
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }

    std::vector<std::vector<uint8_t>> data_chunks;
    bool rd_ok = false;
    CLIO_CO_AWAIT(ReconstructRow(r, /*exclude=*/failed, rctx, data_chunks,
                                 rd_ok));
    if (!rd_ok) {
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }

    ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kChunkLen);
    if (buf.IsNull()) {
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    if (is_data) {
      std::memcpy(buf.ptr_, data_chunks[static_cast<size_t>(idx)].data(),
                  kChunkLen);
    } else {
      std::vector<const uint8_t *> ptrs(static_cast<size_t>(k_));
      for (int c = 0; c < k_; ++c) {
        ptrs[static_cast<size_t>(c)] = data_chunks[static_cast<size_t>(c)].data();
      }
      rs_->EncodeParityShard(idx, ptrs, kChunkLen,
                             reinterpret_cast<uint8_t *>(buf.ptr_));
    }

    auto fut = member_clients_[static_cast<size_t>(failed)].AsyncWrite(
        MemberQuery(), MemberBlocks(ChunkOffset(r), kChunkLen),
        buf.shm_.template Cast<void>(), kChunkLen);
    CLIO_CO_AWAIT(fut);
    const bool ok =
        (fut->return_code_ == 0) && (fut->bytes_written_ == kChunkLen);
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
  members_[static_cast<size_t>(failed)].state_ = ec::EcState::kActive;

  // Stamp the fresh recovery member's superblock with this array's identity.
  {
    bool wr_ok = false;
    CLIO_CO_AWAIT(WriteSuperblock(static_cast<size_t>(failed), rctx, wr_ok));
    if (!wr_ok) {
      HLOG(kWarning,
           "safe_bdev RecoverBdev: superblock write failed for recovered "
           "member {} (data reconstructed; will be re-stamped on next attach)",
           failed);
    }
  }

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
  if (parity_level_ == 0) {
    // No parity configured: nothing to protect. Drop pending marks.
    std::lock_guard<std::mutex> g(row_mu_);
    dirty_rows_.clear();
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }

  // Snapshot a batch of dirty rows (max_batch_==0 drains all). Rows are erased
  // only AFTER their parity is durably written, so an observer that sees an
  // empty dirty set knows every row is protected (single periodic builder,
  // rescheduled only after completion). A row that cannot be built (a data
  // member is down) is left dirty for a later pass.
  std::vector<chi::u64> batch;
  {
    std::lock_guard<std::mutex> g(row_mu_);
    for (chi::u64 r : dirty_rows_) {
      batch.push_back(r);
      if (task->max_batch_ != 0 &&
          batch.size() >= static_cast<size_t>(task->max_batch_)) {
        break;
      }
    }
  }

  auto *ipc = CLIO_IPC;
  chi::u32 built = 0;
  for (chi::u64 r : batch) {
    // Read the k FULL data chunks at this row offset (all data members must be
    // active to build parity over the full chunks).
    std::vector<std::vector<uint8_t>> data_chunks(
        static_cast<size_t>(k_), std::vector<uint8_t>(kChunkLen, 0));
    bool rd_ok = true;
    for (int c = 0; c < k_; ++c) {
      if (members_[static_cast<size_t>(c)].state_ != ec::EcState::kActive) {
        rd_ok = false;
        break;
      }
      bool one = false;
      CLIO_CO_AWAIT(ReadSegment(static_cast<size_t>(c), ChunkOffset(r),
                                data_chunks[static_cast<size_t>(c)].data(),
                                kChunkLen, rctx, one));
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
      ptrs[static_cast<size_t>(c)] = data_chunks[static_cast<size_t>(c)].data();
    }

    // (Re)compute and write each active parity member's chunk for this row.
    bool wr_ok = true;
    for (int j = 0; j < static_cast<int>(parity_level_); ++j) {
      const size_t mi = static_cast<size_t>(k_ + j);
      if (members_[mi].state_ != ec::EcState::kActive) {
        continue;
      }
      ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kChunkLen);
      if (buf.IsNull()) {
        wr_ok = false;
        break;
      }
      rs_->EncodeParityShard(j, ptrs, kChunkLen,
                             reinterpret_cast<uint8_t *>(buf.ptr_));
      auto fut = member_clients_[mi].AsyncWrite(
          MemberQuery(), MemberBlocks(ChunkOffset(r), kChunkLen),
          buf.shm_.template Cast<void>(), kChunkLen);
      CLIO_CO_AWAIT(fut);
      wr_ok = (fut->return_code_ == 0) && (fut->bytes_written_ == kChunkLen);
      ipc->FreeBuffer(buf);
      if (!wr_ok) {
        break;
      }
    }
    if (!wr_ok) {
      continue;  // leave dirty; retry next pass
    }
    {
      std::lock_guard<std::mutex> g(row_mu_);
      dirty_rows_.erase(r);
    }
    ++built;
  }
  if (built > 0) {
    HLOG(kDebug, "safe_bdev BuildParity: built parity for {} row(s)", built);
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
      std::lock_guard<std::mutex> g(row_mu_);
      dirty = static_cast<chi::u32>(dirty_rows_.size());
    }
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(7);
    pk.pack("pool_name");     pk.pack(pool_name_);
    pk.pack("max_failures");  pk.pack(max_failures_);
    pk.pack("k");             pk.pack(static_cast<chi::u32>(k_));
    pk.pack("parity_level");  pk.pack(parity_level_);
    pk.pack("num_rows");      pk.pack(num_rows_);
    pk.pack("dirty_rows");    pk.pack(dirty);
    pk.pack("reattached_members"); pk.pack(reattached_members_);
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

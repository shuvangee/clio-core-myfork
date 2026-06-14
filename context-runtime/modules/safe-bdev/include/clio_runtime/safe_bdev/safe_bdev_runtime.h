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

#ifndef SAFE_BDEV_RUNTIME_H_
#define SAFE_BDEV_RUNTIME_H_

#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/comutex.h>
#include <clio_runtime/bdev/bdev_client.h>
#include <clio_runtime/bdev/bdev_runtime.h>  // bdev::Heap, bdev::GlobalBlockMap

#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include <clio_ctp/io/io_error.h>  // ctp::IoError, ctp::IsFatalDevice

#include "ec/ec_array.h"  // ec::EcRole, ec::EcState, ec::ReedSolomon
#include "safe_bdev_client.h"
#include "safe_bdev_superblock.h"  // MemberSuperblock, kMemberSuperblockMagic
#include "safe_bdev_tasks.h"

/**
 * Runtime container for safe_bdev ChiMod.
 *
 * safe_bdev presents the bdev task interface (AllocateBlocks/FreeBlocks/Write/
 * Read/GetStats) over a growable array of member bdevs using append-only
 * RAID-0 stripe GROUPS on top of a dedicated-parity model:
 *
 *   - data_members_   are DATA members (RAID-0 striped). The vector GROWS when
 *                     a data drive is added via AddBdev(as_data).
 *   - parity_members_ are the m dedicated PARITY members (appended via
 *                     AddBdev(as_parity)); they are SHARED across all groups.
 *
 * A GROUP g fixes a stripe width k_g = the number of data drives that existed
 * when the group opened, and owns a contiguous band of physical ROWS
 * [first_row_g, first_row_g + num_rows_g). Group 0 covers rows [0, R0); group 1
 * covers [R0, R1); etc. (a "row" is one kChunkLen slot at the same member
 * offset on every member). Within a group the logical space is striped RAID-0
 * over that group's k_g data drives in fixed-size kChunkLen units. Because each
 * group's width is FROZEN at open time, adding a data drive opens a NEW wider
 * group and never reshuffles or re-encodes existing groups' data/parity.
 *
 * For each global ROW r, parity member j holds the Reed-Solomon parity shard j
 * computed (with that row's owning group's rs_g) over the k_g FULL data chunks
 * of that row. Parity is built off the write path (deferred to a periodic
 * BuildParity task over a dirty-ROW set keyed by GLOBAL row), and reads
 * reconstruct a down data member's chunk on demand (decode from the row's
 * survivors under the owning group's code).
 */

namespace clio::run::safe_bdev {

/**
 * Runtime container for safe_bdev operations.
 */
class Runtime : public chi::Container {
 public:
  // Required typedef for CLIO_TASK_CC macro and autogen dispatcher.
  using CreateParams = clio::run::safe_bdev::CreateParams;

  Runtime()
      : max_failures_(1),
        parity_level_(0),
        total_rows_(0),
        max_phys_rows_(0),
        reattached_members_(0) {}
  ~Runtime() override = default;

  /** RAID-0 stripe unit (data shard / parity chunk length) in bytes. */
  static constexpr chi::u64 kChunkLen = 65536;

  /**
   * Reserved superblock area at the front of every member bdev (absolute
   * offset 0). The member's usable region begins at offset kSuperblockSize; a
   * member's chunk for row r is at absolute offset kSuperblockSize +
   * r*kChunkLen.
   */
  static constexpr chi::u64 kSuperblockSize = 65536;

  /** Background parity-builder poll period (microseconds): 50 ms. */
  static constexpr double kBuildParityPeriodUs = 50000.0;

  /**
   * Sanity bound on the number of append-only stripe groups (one per AddBdev
   * -as-data, plus the create group). Groups are sized DYNAMICALLY — the current
   * (widest) group spans all remaining physical rows, and adding a data drive
   * freezes it at its high-water mark and opens a new group over what's left.
   * So a single-group array uses the FULL member capacity; capacity is consumed
   * only as groups actually fill. This cap just bounds metadata growth.
   */
  static constexpr chi::u32 kMaxGroups = 64;

  /**
   * Get live task statistics for this task instance.
   */
  chi::TaskStat GetTaskStats(const chi::Task *task) const override;

  //==========================================================================
  // Method handlers
  //==========================================================================

  /** Create the container (Method::kCreate). */
  chi::TaskResume Create(ctp::ipc::FullPtr<CreateTask> task,
                         chi::RunContext &ctx);

  /** Allocate multiple blocks (Method::kAllocateBlocks). */
  chi::TaskResume AllocateBlocks(ctp::ipc::FullPtr<AllocateBlocksTask> task,
                                 chi::RunContext &ctx);

  /** Free data blocks (Method::kFreeBlocks). */
  chi::TaskResume FreeBlocks(ctp::ipc::FullPtr<FreeBlocksTask> task,
                             chi::RunContext &ctx);

  /** Write data (Method::kWrite). */
  chi::TaskResume Write(ctp::ipc::FullPtr<WriteTask> task,
                        chi::RunContext &ctx);

  /** Read data (Method::kRead). */
  chi::TaskResume Read(ctp::ipc::FullPtr<ReadTask> task, chi::RunContext &ctx);

  /** Get performance statistics (Method::kGetStats). */
  chi::TaskResume GetStats(ctp::ipc::FullPtr<GetStatsTask> task,
                           chi::RunContext &ctx);

  /** Add a member bdev (Method::kAddBdev). */
  chi::TaskResume AddBdev(ctp::ipc::FullPtr<AddBdevTask> task,
                          chi::RunContext &ctx);

  /** Remove a member bdev (Method::kRemoveBdev). */
  chi::TaskResume RemoveBdev(ctp::ipc::FullPtr<RemoveBdevTask> task,
                             chi::RunContext &ctx);

  /** Recover a failed member bdev (Method::kRecoverBdev). */
  chi::TaskResume RecoverBdev(ctp::ipc::FullPtr<RecoverBdevTask> task,
                              chi::RunContext &ctx);

  /** Build/raise parity for dirty rows (Method::kBuildParity). */
  chi::TaskResume BuildParity(ctp::ipc::FullPtr<BuildParityTask> task,
                              chi::RunContext &ctx);

  /** Monitor container state (Method::kMonitor). */
  chi::TaskResume Monitor(ctp::ipc::FullPtr<MonitorTask> task,
                          chi::RunContext &rctx);

  /** Destroy the container (Method::kDestroy). */
  chi::TaskResume Destroy(ctp::ipc::FullPtr<DestroyTask> task,
                          chi::RunContext &ctx);

  //==========================================================================
  // Required virtual methods from chi::Container
  //==========================================================================

  void Init(const chi::PoolId &pool_id, const std::string &pool_name,
            chi::u32 container_id = 0) override;

  chi::TaskResume Run(chi::u32 method, ctp::ipc::FullPtr<chi::Task> task_ptr,
                      chi::RunContext &rctx) override;

  chi::u64 GetWorkRemaining() const override;

  void SaveTask(chi::u32 method, chi::SaveTaskArchive &archive,
                ctp::ipc::FullPtr<chi::Task> task_ptr) override;

  void LoadTask(chi::u32 method, chi::LoadTaskArchive &archive,
                ctp::ipc::FullPtr<chi::Task> task_ptr) override;

  ctp::ipc::FullPtr<chi::Task> AllocLoadTask(
      chi::u32 method, chi::LoadTaskArchive &archive) override;

  void LocalLoadTask(chi::u32 method, chi::DefaultLoadArchive &archive,
                     ctp::ipc::FullPtr<chi::Task> task_ptr) override;

  ctp::ipc::FullPtr<chi::Task> LocalAllocLoadTask(
      chi::u32 method, chi::DefaultLoadArchive &archive) override;

  void LocalSaveTask(chi::u32 method, chi::DefaultSaveArchive &archive,
                     ctp::ipc::FullPtr<chi::Task> task_ptr) override;

  ctp::ipc::FullPtr<chi::Task> NewCopyTask(
      chi::u32 method, ctp::ipc::FullPtr<chi::Task> orig_task_ptr,
      bool deep) override;

  ctp::ipc::FullPtr<chi::Task> NewTask(chi::u32 method) override;

  void Aggregate(chi::u32 method, ctp::ipc::FullPtr<chi::Task> orig_task,
                 const ctp::ipc::FullPtr<chi::Task> &replica_task) override;

  void DelTask(chi::u32 method,
               ctp::ipc::FullPtr<chi::Task> task_ptr) override;

 private:
  // Per-member runtime bookkeeping. role_ (DATA vs PARITY) and index_ are FIXED
  // for the member's lifetime — no rotation. DATA members live in
  // data_members_ (index_ == data column == position in the vector); PARITY
  // members live in parity_members_ (index_ == parity row j == position). A
  // member's chunk for GLOBAL row r lives at absolute offset
  // kSuperblockSize + r*kChunkLen.
  struct MemberSlot {
    chi::PoolId pool_id_;
    std::string pool_name_;
    chi::u32 node_id_ = 0;
    ec::EcRole role_ = ec::EcRole::kData;
    ec::EcState state_ = ec::EcState::kActive;
    int index_ = -1;  // data column c (DATA) or parity row j (PARITY)
  };

  // An append-only RAID-0 stripe group. A group fixes its data-drive width k_g
  // (the number of data members that existed when it opened) and owns a band of
  // global rows [first_row_, first_row_ + num_rows_). Its logical address range
  // is [logical_base_, logical_base_ + k_g*num_rows_*kChunkLen); a per-group
  // allocator (block_map_ over the free list + heap_ for the bump region) hands
  // out logical offsets inside that range (the heap returns offsets in
  // [0, span); we add logical_base_ so global offsets are unique, and subtract
  // it on free). rs_ is ReedSolomon(k_g, max_failures).
  struct Group {
    int k_ = 0;                  // data-drive count frozen at open (k_g)
    chi::u64 first_row_ = 0;     // first global row owned by this group
    chi::u64 num_rows_ = 0;      // rows of kChunkLen reserved per member
    chi::u64 logical_base_ = 0;  // start of this group's logical byte range
    chi::u64 logical_span_ = 0;  // k_g * num_rows_ * kChunkLen
    std::unique_ptr<ec::ReedSolomon> rs_;
    std::unique_ptr<clio::run::bdev::GlobalBlockMap> block_map_;
    std::unique_ptr<clio::run::bdev::Heap> heap_;
    std::atomic<chi::u64> allocated_bytes_{0};

    chi::u64 LogicalEnd() const { return logical_base_ + logical_span_; }
    chi::u64 LastRow() const { return first_row_ + num_rows_; }  // exclusive
    bool ContainsOffset(chi::u64 off) const {
      return off >= logical_base_ && off < LogicalEnd();
    }
    bool ContainsRow(chi::u64 row) const {
      return row >= first_row_ && row < LastRow();
    }
  };

  // Client for making calls back to this ChiMod.
  Client client_;

  // EC / membership state. DATA and PARITY members are kept in SEPARATE vectors
  // so a data drive can be appended (growing data_members_) without disturbing
  // parity column indices. Each member vector is index-aligned with its client
  // vector. groups_ is append-only: groups_.back() is the CURRENT (widest)
  // group that new allocations come from.
  std::vector<MemberSlot> data_members_;    // index_ == data column
  std::vector<MemberSlot> parity_members_;  // index_ == parity row j
  std::vector<clio::run::bdev::Client> data_clients_;
  std::vector<clio::run::bdev::Client> parity_clients_;
  // Append-only stripe groups, held by pointer so the std::atomic inside each
  // Group never moves when the vector grows.
  std::vector<std::unique_ptr<Group>> groups_;
  chi::u32 max_failures_;           // Fault-tolerance target (M == m_max)
  chi::u32 parity_level_;           // Parity members added so far (m)
  chi::u64 total_rows_;             // Physical rows available (== max_phys_rows_)
  chi::u64 max_phys_rows_;          // Physical rows available per member
  chi::u32 reattached_members_;     // Members recognized as already ours at Create

  // Async-parity bookkeeping. Write writes data chunks immediately and records
  // each touched GLOBAL ROW as dirty (parity not yet current); BuildParity
  // drains the dirty set and (re)computes all parity rows under each row's
  // owning group's code. written_rows_ tracks every row that holds data so a
  // parity-level increase (AddBdev as_parity) can re-dirty exactly those.
  // Guarded by row_mu_ (never held across a co_await). A row is safe to
  // reconstruct only when NOT dirty — degraded reads / recovery refuse a dirty
  // (unprotected) row.
  std::set<chi::u64> dirty_rows_;
  std::set<chi::u64> written_rows_;
  mutable std::mutex row_mu_;

  /** Mark a row as holding data and needing (re)parity. */
  void MarkRowDirty(chi::u64 row) {
    std::lock_guard<std::mutex> g(row_mu_);
    written_rows_.insert(row);
    dirty_rows_.insert(row);
  }
  /** True if the row's parity is not yet current (unprotected). */
  bool IsRowDirty(chi::u64 row) const {
    std::lock_guard<std::mutex> g(row_mu_);
    return dirty_rows_.count(row) != 0;
  }

  //==========================================================================
  // Per-group RAID-0 address mapping. A logical byte offset L belongs to the
  // group g whose logical range contains it (FindGroupByOffset). With
  // local = L - g.logical_base_:
  //   chunk     = local / kChunkLen;          within     = local % kChunkLen
  //   data_col  = chunk % k_g;                row_in_grp = chunk / k_g
  //   global_row = g.first_row_ + row_in_grp
  // physical offset on DATA member data_col = kSuperblockSize +
  // global_row*kChunkLen + within. Parity member j's chunk for the same global
  // row is at the same physical offset kSuperblockSize + global_row*kChunkLen.
  //==========================================================================

  /** Absolute member-pool offset of global row `r`'s chunk start. */
  static chi::u64 ChunkOffset(chi::u64 row) {
    return kSuperblockSize + row * kChunkLen;
  }

  /** The current (widest, last-opened) group new allocations come from. */
  Group &CurrentGroup() { return *groups_.back(); }

  /** Index of the group whose logical range contains byte offset `off`, or
   *  -1 if none. */
  int FindGroupByOffset(chi::u64 off) const {
    for (size_t i = 0; i < groups_.size(); ++i) {
      if (groups_[i]->ContainsOffset(off)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  /** Index of the group that owns global row `row`, or -1 if none. */
  int FindGroupByRow(chi::u64 row) const {
    for (size_t i = 0; i < groups_.size(); ++i) {
      if (groups_[i]->ContainsRow(row)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  /** Per-member pool query (members are independent local bdev pools). */
  chi::PoolQuery MemberQuery() const { return chi::PoolQuery::Local(); }

  /**
   * Automatic down-detection for a DATA member. Inspect a member-bdev future's
   * io_error_ after a chunk I/O completes: a fatal device error (DeviceFault /
   * Disconnected) ejects data member `col` (state_ = kFaulty) so the
   * degraded-read / reconstruct path takes over. A TRANSIENT error never faults
   * the member.
   */
  void MaybeFaultData(size_t col, chi::u32 io_error) {
    const auto e = static_cast<ctp::IoError>(io_error);
    if (ctp::IsFatalDevice(e) && col < data_members_.size() &&
        data_members_[col].state_ == ec::EcState::kActive) {
      data_members_[col].state_ = ec::EcState::kFaulty;
      HLOG(kWarning,
           "safe_bdev: data member {} auto-faulted on fatal device error '{}' "
           "(io_error={})",
           col, ctp::IoErrorName(e), io_error);
    }
  }

  /** Automatic down-detection for a PARITY member (parity row j). */
  void MaybeFaultParity(size_t j, chi::u32 io_error) {
    const auto e = static_cast<ctp::IoError>(io_error);
    if (ctp::IsFatalDevice(e) && j < parity_members_.size() &&
        parity_members_[j].state_ == ec::EcState::kActive) {
      parity_members_[j].state_ = ec::EcState::kFaulty;
      HLOG(kWarning,
           "safe_bdev: parity member {} auto-faulted on fatal device error "
           "'{}' (io_error={})",
           j, ctp::IoErrorName(e), io_error);
    }
  }

  //==========================================================================
  // EC / I/O helpers (defined in safe_bdev_runtime.cc; run inside task fibers).
  //==========================================================================

  /** Block list addressing [offset, offset+len) on a member pool. */
  chi::priv::vector<clio::run::bdev::Block> MemberBlocks(chi::u64 offset,
                                                         chi::u64 len) const;

  /**
   * AsyncWrite `len` bytes from host buffer `src` to DATA member `col` at
   * absolute member-pool offset `offset`. Auto-faults the member on a fatal
   * io_error.
   */
  chi::TaskResume WriteDataSegment(size_t col, chi::u64 offset,
                                   const uint8_t *src, chi::u64 len,
                                   chi::RunContext &rctx, bool &ok);

  /**
   * AsyncRead `len` bytes at absolute member-pool offset `offset` from DATA
   * member `col` into host buffer `dst`. Auto-faults the member on a fatal
   * io_error.
   */
  chi::TaskResume ReadDataSegment(size_t col, chi::u64 offset, uint8_t *dst,
                                  chi::u64 len, chi::RunContext &rctx, bool &ok);

  /**
   * Reconstruct the FULL kChunkLen chunk of DATA member `data_col` for global
   * row `row` (owned by group `g`) by gathering k_g survivors among that
   * group's k_g data + m parity members' chunks at that row (excluding faulty /
   * `exclude_col` data member), DecodeData under g.rs_. `out` receives
   * kChunkLen bytes. Returns false if too few survivors.
   */
  chi::TaskResume ReconstructDataChunk(const Group &g, chi::u64 row,
                                       int data_col, int exclude_col,
                                       chi::RunContext &rctx,
                                       std::vector<uint8_t> &out, bool &ok);

  /**
   * Reconstruct ALL k_g data chunks for global row `row` (owned by group `g`)
   * from active survivors (DecodeData under g.rs_), optionally excluding data
   * column `exclude_col`. `out` receives k_g buffers of kChunkLen bytes.
   * Returns false on too few survivors.
   */
  chi::TaskResume ReconstructRow(const Group &g, chi::u64 row, int exclude_col,
                                 chi::RunContext &rctx,
                                 std::vector<std::vector<uint8_t>> &out,
                                 bool &ok);

  /**
   * Serialize this array's identity for a member into a zero-padded
   * kSuperblockSize buffer and AsyncWrite it to that member at absolute offset
   * 0. `is_parity` selects parity_members_/parity_clients_ vs data; `idx` is
   * the position in that vector. Returns false on I/O failure.
   */
  chi::TaskResume WriteSuperblock(bool is_parity, size_t idx,
                                  chi::RunContext &rctx, bool &ok);

  /**
   * AsyncRead kSuperblockSize bytes at absolute offset 0 from a member, parse
   * the superblock into `sb`, and set `present` = (magic matches && checksum
   * valid). A blank member reads back zeros => present == false. `is_parity`
   * selects the member vector; `idx` is its position.
   */
  chi::TaskResume ReadSuperblock(bool is_parity, size_t idx,
                                 chi::RunContext &rctx, MemberSuperblock &sb,
                                 bool &present, bool &ok);

  /** Open and initialize a new group with the given width / row band. The
   *  group's rs_, block_map_ and heap_ are constructed; appended to groups_. */
  void OpenGroup(int k, chi::u64 first_row, chi::u64 num_rows,
                 chi::u64 logical_base);
};

}  // namespace clio::run::safe_bdev

#endif  // SAFE_BDEV_RUNTIME_H_

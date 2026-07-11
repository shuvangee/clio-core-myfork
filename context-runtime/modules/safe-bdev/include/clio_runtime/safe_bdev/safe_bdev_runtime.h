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
#include <clio_runtime/bdev/transports/block_allocator.h>  // bdev::Heap, bdev::GlobalBlockMap
#include <clio_runtime/bdev/bdev_alloc_log.h>  // bdev::AllocatorLog (reused WAL)

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
class Runtime : public clio::run::Container {
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
  static constexpr clio::run::u64 kChunkLen = 65536;

  /**
   * Reserved superblock area at the front of every member bdev (absolute
   * offset 0). The member's usable region begins at offset kSuperblockSize; a
   * member's chunk for row r is at absolute offset kSuperblockSize +
   * r*kChunkLen.
   */
  static constexpr clio::run::u64 kSuperblockSize = 65536;

  /** Background parity-builder poll period (microseconds): 50 ms. */
  static constexpr double kBuildParityPeriodUs = 50000.0;

  /** Allocator-WAL flush/compact poll period (microseconds): 50 ms. */
  static constexpr double kFlushAllocLogPeriodUs = 50000.0;

  /** Compaction policy: rewrite the WAL once on-disk records exceed
   *  max(kMinCompactRecords, live * kCompactGrowthFactor). Mirrors bdev. */
  static constexpr clio::run::u64 kMinCompactRecords = 1024;
  static constexpr clio::run::u64 kCompactGrowthFactor = 4;

  /**
   * Sanity bound on the number of append-only stripe groups (one per AddBdev
   * -as-data, plus the create group). Groups are sized DYNAMICALLY — the current
   * (widest) group spans all remaining physical rows, and adding a data drive
   * freezes it at its high-water mark and opens a new group over what's left.
   * So a single-group array uses the FULL member capacity; capacity is consumed
   * only as groups actually fill. This cap just bounds metadata growth.
   */
  static constexpr clio::run::u32 kMaxGroups = 64;

  /**
   * Get live task statistics for this task instance.
   */
  clio::run::TaskStat GetTaskStats(const clio::run::Task *task) const override;

  //==========================================================================
  // Method handlers
  //==========================================================================

  /** Create the container (Method::kCreate). */
  clio::run::TaskResume Create(clio::run::shared_ptr<CreateTask> &task);

  /** Allocate multiple blocks (Method::kAllocateBlocks). */
  clio::run::TaskResume AllocateBlocks(clio::run::shared_ptr<AllocateBlocksTask> &task);

  /** Free data blocks (Method::kFreeBlocks). */
  clio::run::TaskResume FreeBlocks(clio::run::shared_ptr<FreeBlocksTask> &task);

  /** Write data (Method::kWrite). */
  clio::run::TaskResume Write(clio::run::shared_ptr<WriteTask> &task);

  /** Read data (Method::kRead). */
  clio::run::TaskResume Read(clio::run::shared_ptr<ReadTask> &task);

  /** Get performance statistics (Method::kGetStats). */
  clio::run::TaskResume GetStats(clio::run::shared_ptr<GetStatsTask> &task);

  /** Add a member bdev (Method::kAddBdev). */
  clio::run::TaskResume AddBdev(clio::run::shared_ptr<AddBdevTask> &task);

  /** Remove a member bdev (Method::kRemoveBdev). */
  clio::run::TaskResume RemoveBdev(clio::run::shared_ptr<RemoveBdevTask> &task);

  /** Recover a failed member bdev (Method::kRecoverBdev). */
  clio::run::TaskResume RecoverBdev(clio::run::shared_ptr<RecoverBdevTask> &task);

  /** Build/raise parity for dirty rows (Method::kBuildParity). */
  clio::run::TaskResume BuildParity(clio::run::shared_ptr<BuildParityTask> &task);

  /** Flush/compact the persistent allocator log (Method::kFlushAllocLog). */
  clio::run::TaskResume FlushAllocLog(clio::run::shared_ptr<FlushAllocLogTask> &task);

  /** Monitor container state (Method::kMonitor). */
  clio::run::TaskResume Monitor(clio::run::shared_ptr<MonitorTask> &task);

  /** Destroy the container (Method::kDestroy). */
  clio::run::TaskResume Destroy(clio::run::shared_ptr<DestroyTask> &task);

  //==========================================================================
  // Required virtual methods from clio::run::Container
  //==========================================================================

  void Init(const clio::run::PoolId &pool_id, const std::string &pool_name,
            clio::run::u32 container_id = 0) override;

  clio::run::TaskResume Run(clio::run::u32 method,
                      clio::run::shared_ptr<clio::run::Task> task_ptr) override;

  clio::run::u64 GetWorkRemaining() const override;

  void SaveTask(clio::run::u32 method, clio::run::SaveTaskArchive &archive,
                clio::run::shared_ptr<clio::run::Task> &task_ptr) override;

  void LoadTask(clio::run::u32 method, clio::run::LoadTaskArchive &archive,
                clio::run::shared_ptr<clio::run::Task> &task_ptr) override;

  clio::run::shared_ptr<clio::run::Task> AllocLoadTask(
      clio::run::u32 method, clio::run::LoadTaskArchive &archive) override;

  void LocalLoadTask(clio::run::u32 method, clio::run::DefaultLoadArchive &archive,
                     clio::run::shared_ptr<clio::run::Task> &task_ptr) override;

  clio::run::shared_ptr<clio::run::Task> LocalAllocLoadTask(
      clio::run::u32 method, clio::run::DefaultLoadArchive &archive) override;

  void LocalSaveTask(clio::run::u32 method, clio::run::DefaultSaveArchive &archive,
                     clio::run::shared_ptr<clio::run::Task> &task_ptr) override;

  clio::run::shared_ptr<clio::run::Task> NewCopyTask(
      clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> &orig_task_ptr,
      bool deep) override;

  clio::run::shared_ptr<clio::run::Task> NewTask(clio::run::u32 method) override;

  void AggregateOut(clio::run::u32 method, clio::run::shared_ptr<clio::run::Task> &orig_task,
                 const clio::run::shared_ptr<clio::run::Task> &replica_task) override;

 private:
  // Per-member runtime bookkeeping. role_ (DATA vs PARITY) and index_ are FIXED
  // for the member's lifetime — no rotation. DATA members live in
  // data_members_ (index_ == data column == position in the vector); PARITY
  // members live in parity_members_ (index_ == parity row j == position). A
  // member's chunk for GLOBAL row r lives at absolute offset
  // kSuperblockSize + r*kChunkLen.
  struct MemberSlot {
    clio::run::PoolId pool_id_;
    std::string pool_name_;
    clio::run::u32 node_id_ = 0;
    ec::EcRole role_ = ec::EcRole::kData;
    ec::EcState state_ = ec::EcState::kActive;
    int index_ = -1;  // data column c (DATA) or parity row j (PARITY)
    // True while this member's shards are being rebuilt onto pool_id_ (a
    // recovery target seated by RecoverBdev). Persisted in the member manifest
    // so an interrupted recovery is RESUMED on restart: the member stays
    // non-active (excluded from reads -> degraded path) until the rebuild
    // completes. RebuildMember is idempotent, so re-running after a crash is
    // safe.
    bool recovering_ = false;
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
    clio::run::u64 first_row_ = 0;     // first global row owned by this group
    clio::run::u64 num_rows_ = 0;      // rows of kChunkLen reserved per member
    clio::run::u64 logical_base_ = 0;  // start of this group's logical byte range
    clio::run::u64 logical_span_ = 0;  // k_g * num_rows_ * kChunkLen
    std::unique_ptr<ec::ReedSolomon> rs_;
    std::unique_ptr<clio::run::bdev::GlobalBlockMap> block_map_;
    std::unique_ptr<clio::run::bdev::Heap> heap_;
    std::atomic<clio::run::u64> allocated_bytes_{0};

    clio::run::u64 LogicalEnd() const { return logical_base_ + logical_span_; }
    clio::run::u64 LastRow() const { return first_row_ + num_rows_; }  // exclusive
    bool ContainsOffset(clio::run::u64 off) const {
      return off >= logical_base_ && off < LogicalEnd();
    }
    bool ContainsRow(clio::run::u64 row) const {
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
  // Persistent allocator-state log (WAL). REUSED from the bdev module. Each
  // group's per-group allocator state is namespaced by the group's INDEX in
  // groups_ (group_id 0,1,2,...; stable because groups_ is append-only). The
  // append-only group geometry itself is persisted via LogGroupOpen /
  // LogGroupFreeze. Empty path => disabled (every API is a no-op), so the
  // pre-WAL behaviour is preserved unchanged.
  clio::run::bdev::AllocatorLog alloc_log_;
  // Durable member manifest path (== alloc_log_path + ".members"; empty when
  // the alloc log is disabled). Unlike the config member list -- which only
  // names the STARTUP data members -- the manifest records the CURRENT full
  // membership (data + parity, including runtime AddBdev drives and recovery
  // replacements) plus each member's state and recovery flag, so Create()
  // restores the real array on restart and resumes any interrupted recovery.
  std::string members_manifest_path_;
  clio::run::u32 max_failures_;           // Fault-tolerance target (M == m_max)
  clio::run::u32 parity_level_;           // Parity members added so far (m)
  clio::run::u64 total_rows_;             // Physical rows available (== max_phys_rows_)
  clio::run::u64 max_phys_rows_;          // Physical rows available per member
  clio::run::u32 reattached_members_;     // Members recognized as already ours at Create

  //==========================================================================
  // Recovery observability. RecoverBdev rebuilds a failed member's shard for
  // EVERY global row it participates in; each such row is one "recovery
  // operation". These atomics let Monitor() -- and the context-visualizer
  // safe-bdev dashboard -- report live rebuild progress: ops IN FLIGHT (the row
  // whose reconstructed shard is being written right now) vs REMAINING (rows not
  // yet rebuilt). They are reset at the start of every RecoverBdev call.
  // recovery_active_ gates whether a rebuild is underway; recovering_is_parity_
  // / recovering_index_ identify which member is being rebuilt so the dashboard
  // can flag it. Atomic because Monitor() may run on a different worker fiber
  // than the in-progress RecoverBdev.
  //==========================================================================
  std::atomic<clio::run::u64> recovery_ops_total_{0};      // rows to rebuild this run
  std::atomic<clio::run::u64> recovery_ops_completed_{0};  // rows rebuilt so far
  std::atomic<clio::run::u64> recovery_ops_in_flight_{0};  // row mid-write right now
  std::atomic<clio::run::u32> recovery_active_{0};         // 1 while a rebuild runs
  std::atomic<clio::run::u32> recovering_is_parity_{0};    // member role being rebuilt
  std::atomic<int> recovering_index_{-1};                  // member index being rebuilt

  // Async-parity bookkeeping. Write writes data chunks immediately and records
  // each touched GLOBAL ROW as dirty (parity not yet current); BuildParity
  // drains the dirty set and (re)computes all parity rows under each row's
  // owning group's code. written_rows_ tracks every row that holds data so a
  // parity-level increase (AddBdev as_parity) can re-dirty exactly those.
  // Guarded by row_mu_ (never held across a co_await). A row is safe to
  // reconstruct only when NOT dirty — degraded reads / recovery refuse a dirty
  // (unprotected) row.
  std::set<clio::run::u64> dirty_rows_;
  std::set<clio::run::u64> written_rows_;
  mutable std::mutex row_mu_;

  /** Mark a row as holding data and needing (re)parity. */
  void MarkRowDirty(clio::run::u64 row) {
    std::lock_guard<std::mutex> g(row_mu_);
    written_rows_.insert(row);
    dirty_rows_.insert(row);
  }
  /** True if the row's parity is not yet current (unprotected). */
  bool IsRowDirty(clio::run::u64 row) const {
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
  static clio::run::u64 ChunkOffset(clio::run::u64 row) {
    return kSuperblockSize + row * kChunkLen;
  }

  /** The current (widest, last-opened) group new allocations come from. */
  Group &CurrentGroup() { return *groups_.back(); }

  /** Index of the group whose logical range contains byte offset `off`, or
   *  -1 if none. */
  int FindGroupByOffset(clio::run::u64 off) const {
    for (size_t i = 0; i < groups_.size(); ++i) {
      if (groups_[i]->ContainsOffset(off)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  /** Index of the group that owns global row `row`, or -1 if none. */
  int FindGroupByRow(clio::run::u64 row) const {
    for (size_t i = 0; i < groups_.size(); ++i) {
      if (groups_[i]->ContainsRow(row)) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  /** Per-member pool query (members are independent local bdev pools). */
  clio::run::PoolQuery MemberQuery() const { return clio::run::PoolQuery::Local(); }

  /**
   * Automatic down-detection for a DATA member. Inspect a member-bdev future's
   * io_error_ after a chunk I/O completes: a fatal device error (DeviceFault /
   * Disconnected) ejects data member `col` (state_ = kFaulty) so the
   * degraded-read / reconstruct path takes over. A TRANSIENT error never faults
   * the member.
   */
  void MaybeFaultData(size_t col, clio::run::u32 io_error) {
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
  void MaybeFaultParity(size_t j, clio::run::u32 io_error) {
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
  clio::run::priv::vector<clio::run::bdev::Block> MemberBlocks(clio::run::u64 offset,
                                                         clio::run::u64 len) const;

  /**
   * AsyncWrite `len` bytes from host buffer `src` to DATA member `col` at
   * absolute member-pool offset `offset`. Auto-faults the member on a fatal
   * io_error.
   */
  clio::run::TaskResume WriteDataSegment(size_t col, clio::run::u64 offset,
                                   const uint8_t *src, clio::run::u64 len,
                                   bool &ok);

  /**
   * AsyncRead `len` bytes at absolute member-pool offset `offset` from DATA
   * member `col` into host buffer `dst`. Auto-faults the member on a fatal
   * io_error.
   */
  clio::run::TaskResume ReadDataSegment(size_t col, clio::run::u64 offset, uint8_t *dst,
                                  clio::run::u64 len, bool &ok);

  /**
   * Reconstruct the FULL kChunkLen chunk of DATA member `data_col` for global
   * row `row` (owned by group `g`) by gathering k_g survivors among that
   * group's k_g data + m parity members' chunks at that row (excluding faulty /
   * `exclude_col` data member), DecodeData under g.rs_. `out` receives
   * kChunkLen bytes. Returns false if too few survivors.
   */
  clio::run::TaskResume ReconstructDataChunk(const Group &g, clio::run::u64 row,
                                       int data_col, int exclude_col,
                                       std::vector<uint8_t> &out, bool &ok);

  /**
   * Reconstruct ALL k_g data chunks for global row `row` (owned by group `g`)
   * from active survivors (DecodeData under g.rs_), optionally excluding data
   * column `exclude_col`. `out` receives k_g buffers of kChunkLen bytes.
   * Returns false on too few survivors.
   */
  clio::run::TaskResume ReconstructRow(const Group &g, clio::run::u64 row, int exclude_col,
                                 std::vector<std::vector<uint8_t>> &out,
                                 bool &ok);

  /**
   * Serialize this array's identity for a member into a zero-padded
   * kSuperblockSize buffer and AsyncWrite it to that member at absolute offset
   * 0. `is_parity` selects parity_members_/parity_clients_ vs data; `idx` is
   * the position in that vector. Returns false on I/O failure.
   */
  clio::run::TaskResume WriteSuperblock(bool is_parity, size_t idx, bool &ok);

  /**
   * AsyncRead kSuperblockSize bytes at absolute offset 0 from a member, parse
   * the superblock into `sb`, and set `present` = (magic matches && checksum
   * valid). A blank member reads back zeros => present == false. `is_parity`
   * selects the member vector; `idx` is its position.
   */
  clio::run::TaskResume ReadSuperblock(bool is_parity, size_t idx,
                                 MemberSuperblock &sb,
                                 bool &present, bool &ok);

  //==========================================================================
  // Durable member manifest (membership + recovery state persistence).
  //==========================================================================

  /** One persisted member record. Mirrors a MemberSlot's durable fields. */
  struct MemberManifestEntry {
    clio::run::u32 role_ = 0;        // ec::EcRole (0=data, 1=parity)
    clio::run::u32 index_ = 0;       // data column / parity row
    clio::run::u32 pool_major_ = 0;  // member bdev pool id
    clio::run::u32 pool_minor_ = 0;
    clio::run::u32 node_id_ = 0;
    clio::run::u32 state_ = 0;        // ec::EcState (0=active,1=faulty,2=removed)
    clio::run::u32 recovering_ = 0;  // 1 if mid-recovery onto (pool_major/minor)
    std::string pool_name_;
  };

  /** Atomically rewrite the member manifest from the live data/parity slots.
   *  No-op when members_manifest_path_ is empty (alloc log disabled). */
  void PersistMemberManifest() const;

  /** Load the member manifest into `out`. Returns false if absent/corrupt. */
  bool LoadMemberManifest(std::vector<MemberManifestEntry> &out) const;

  /** Reconstruct + write EVERY global row this member participates in onto its
   *  (already-seated) client, under each row's owning group's code. Idempotent:
   *  safe to re-run after an interrupted recovery. On return `ok` reports I/O
   *  success and `completed` is false only when the CLIO_SAFE_BDEV_RECOVER_MAX_
   *  ROWS test hook stopped the rebuild early (recovery left in-progress). */
  clio::run::TaskResume RebuildMember(bool is_data, int idx, bool &ok,
                                      bool &completed);

  /** After membership + groups are restored, resume any member left in the
   *  recovering state (crash mid-RecoverBdev). Persists the manifest as members
   *  come back online. */
  clio::run::TaskResume ResumeRecoveries();

  /** Open and initialize a new group with the given width / row band. The
   *  group's rs_, block_map_ and heap_ are constructed; appended to groups_. */
  void OpenGroup(int k, clio::run::u64 first_row, clio::run::u64 num_rows,
                 clio::run::u64 logical_base);

  /**
   * Rebuild group `gi`'s per-group allocator (heap bump + free list) from a
   * recovered live set. `live` offsets are GLOBAL logical offsets, so the
   * group-local offset = global - group.logical_base_. Mirrors bdev's
   * InitializeAllocatorFromLive (Heap::InitFromLive over the local offsets, +
   * GlobalBlockMap::SeedFreeRange for the gaps below the bump). Also sets the
   * group's allocated_bytes_ to the sum of live sizes. Call AFTER OpenGroup so
   * the group's heap_/block_map_ exist.
   */
  void SeedGroupAllocatorFromLive(
      size_t gi, const std::vector<clio::run::bdev::LiveBlock> &live);
};

}  // namespace clio::run::safe_bdev

#endif  // SAFE_BDEV_RUNTIME_H_

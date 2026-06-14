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
 * Read/GetStats) over a fixed array of member bdevs using a RAID-0-data +
 * dedicated-parity layout (single fixed k/m):
 *
 *   - members_[0 .. k-1]   are DATA members (RAID-0 striped).
 *   - members_[k .. k+m-1] are PARITY members (appended via AddBdev).
 *
 * The logical address space is striped over the k data members in fixed-size
 * kChunkLen units (RAID-0). For each ROW r (a kChunkLen slice on every member),
 * parity member j holds the Reed-Solomon parity shard j computed over the k
 * FULL data chunks of that row. Parity is built off the write path (deferred to
 * a periodic BuildParity task over a dirty-ROW set), and reads reconstruct a
 * down data member's chunk on demand (decode from the row's survivors).
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
        k_(0),
        num_rows_(0),
        logical_capacity_(0),
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
  // Per-member runtime bookkeeping. role_ (DATA vs PARITY) and index_ (data
  // column c, or parity row j) are FIXED for the member's lifetime — no
  // rotation, no generations. members_[0..k-1] are DATA (index_ == column),
  // members_[k..k+m-1] are PARITY (index_ == parity row j). A member's chunk
  // for row r lives at absolute offset kSuperblockSize + r*kChunkLen.
  struct MemberSlot {
    chi::PoolId pool_id_;
    std::string pool_name_;
    chi::u32 node_id_ = 0;
    ec::EcRole role_ = ec::EcRole::kData;
    ec::EcState state_ = ec::EcState::kActive;
    int index_ = -1;  // data column c (DATA) or parity row j (PARITY)
  };

  // Client for making calls back to this ChiMod.
  Client client_;

  // EC / membership state.
  std::vector<MemberSlot> members_;  // [0,k) data, [k,k+m) parity
  chi::u32 max_failures_;            // Fault-tolerance target (M == m_max)
  chi::u32 parity_level_;            // Parity members added so far (m)
  int k_;                           // Number of DATA members (fixed)
  chi::u64 num_rows_;               // Rows of kChunkLen reserved per member
  chi::u64 logical_capacity_;       // Usable logical bytes = k*num_rows*kChunkLen
  chi::u32 reattached_members_;     // Members recognized as already ours at Create

  // One Reed-Solomon code (k, max_failures). Data column for data member i is
  // i; parity member j carries global RS shard index k+j.
  std::unique_ptr<ec::ReedSolomon> rs_;

  // Clients for delegating data-plane I/O to member bdevs. members_ and
  // member_clients_ are kept index-aligned.
  std::vector<clio::run::bdev::Client> member_clients_;

  // Real reclaimable allocator over the logical capacity. Reuses bdev's
  // free-list + heap (allocate-from-free-list-else-heap), so freed blocks are
  // reused and allocation sizes are uneven exactly like bdev. Offsets returned
  // are logical-space byte offsets.
  clio::run::bdev::GlobalBlockMap block_map_;
  clio::run::bdev::Heap heap_;
  std::atomic<chi::u64> allocated_bytes_{0};

  // Async-parity bookkeeping. Write writes data chunks immediately and records
  // each touched ROW as dirty (parity not yet current); BuildParity drains the
  // dirty set and (re)computes all parity rows. written_rows_ tracks every row
  // that holds data so a parity-level increase (AddBdev as_parity) can re-dirty
  // exactly those. Guarded by row_mu_ (never held across a co_await). A row is
  // safe to reconstruct only when NOT dirty — degraded reads / recovery refuse
  // a dirty (unprotected) row.
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
  // RAID-0 address mapping. A logical byte offset L maps to:
  //   chunk    = L / kChunkLen;     within = L % kChunkLen
  //   data_col = chunk % k;         row    = chunk / k
  // physical offset on DATA member data_col = kSuperblockSize + row*kChunkLen +
  // within. Parity member j's chunk for row r is at the same physical offset
  // kSuperblockSize + r*kChunkLen.
  //==========================================================================

  /** Absolute member-pool offset of row `r`'s chunk start. */
  static chi::u64 ChunkOffset(chi::u64 row) {
    return kSuperblockSize + row * kChunkLen;
  }

  /** Per-member pool query (members are independent local bdev pools). */
  chi::PoolQuery MemberQuery() const { return chi::PoolQuery::Local(); }

  /**
   * Automatic down-detection. Inspect a member-bdev future's io_error_ after a
   * chunk I/O completes: a fatal device error (DeviceFault / Disconnected)
   * ejects member `mi` (state_ = kFaulty) so the degraded-read / reconstruct
   * path takes over. A TRANSIENT error never faults the member.
   */
  void MaybeFaultMember(size_t mi, chi::u32 io_error) {
    const auto e = static_cast<ctp::IoError>(io_error);
    if (ctp::IsFatalDevice(e) && mi < members_.size() &&
        members_[mi].state_ == ec::EcState::kActive) {
      members_[mi].state_ = ec::EcState::kFaulty;
      HLOG(kWarning,
           "safe_bdev: member {} auto-faulted on fatal device error '{}' "
           "(io_error={})",
           mi, ctp::IoErrorName(e), io_error);
    }
  }

  //==========================================================================
  // EC / I/O helpers (defined in safe_bdev_runtime.cc; run inside task fibers).
  //==========================================================================

  /** Block list addressing [offset, offset+len) on a member pool. */
  chi::priv::vector<clio::run::bdev::Block> MemberBlocks(chi::u64 offset,
                                                         chi::u64 len) const;

  /**
   * AsyncWrite `len` bytes from host buffer `src` to member `mi` at absolute
   * member-pool offset `offset`. Auto-faults the member on a fatal io_error.
   */
  chi::TaskResume WriteSegment(size_t mi, chi::u64 offset, const uint8_t *src,
                               chi::u64 len, chi::RunContext &rctx, bool &ok);

  /**
   * AsyncRead `len` bytes at absolute member-pool offset `offset` from member
   * `mi` into host buffer `dst`. Auto-faults the member on a fatal io_error.
   */
  chi::TaskResume ReadSegment(size_t mi, chi::u64 offset, uint8_t *dst,
                              chi::u64 len, chi::RunContext &rctx, bool &ok);

  /**
   * Reconstruct the FULL kChunkLen chunk of DATA member `data_col` for row
   * `row` by gathering k survivors among the k data + m parity members'
   * chunks at that row (excluding faulty / `exclude`), DecodeData. `out`
   * receives kChunkLen bytes. Returns false if too few survivors.
   */
  chi::TaskResume ReconstructDataChunk(chi::u64 row, int data_col, int exclude,
                                       chi::RunContext &rctx,
                                       std::vector<uint8_t> &out, bool &ok);

  /**
   * Reconstruct ALL k data chunks for row `row` from active survivors
   * (DecodeData), optionally excluding member id `exclude`. `out` receives k
   * buffers of kChunkLen bytes. Returns false on too few survivors.
   */
  chi::TaskResume ReconstructRow(chi::u64 row, int exclude,
                                 chi::RunContext &rctx,
                                 std::vector<std::vector<uint8_t>> &out,
                                 bool &ok);

  /**
   * Serialize this array's identity for member `mi` into a zero-padded
   * kSuperblockSize buffer and AsyncWrite it to that member at absolute
   * offset 0. Returns false on I/O failure.
   */
  chi::TaskResume WriteSuperblock(size_t mi, chi::RunContext &rctx, bool &ok);

  /**
   * AsyncRead kSuperblockSize bytes at absolute offset 0 from member `mi`,
   * parse the superblock into `sb`, and set `present` = (magic matches &&
   * checksum valid). A blank member reads back zeros => present == false.
   */
  chi::TaskResume ReadSuperblock(size_t mi, chi::RunContext &rctx,
                                 MemberSuperblock &sb, bool &present, bool &ok);
};

}  // namespace clio::run::safe_bdev

#endif  // SAFE_BDEV_RUNTIME_H_

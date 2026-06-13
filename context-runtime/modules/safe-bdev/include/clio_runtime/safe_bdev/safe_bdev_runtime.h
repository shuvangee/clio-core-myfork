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

#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "ec/ec_array.h"  // ec::EcRole, ec::EcState, ec::ReedSolomon
#include "safe_bdev_client.h"
#include "safe_bdev_tasks.h"

/**
 * Runtime container for safe_bdev ChiMod
 *
 * Provides a declustered, erasure-coded block device built on top of one or
 * more member bdevs. The erasure-coding internals are stubbed for issue #543;
 * the data plane currently delegates to a single primary member bdev so the
 * device is a working (degenerate, no-parity) device that compiles and runs.
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
        current_parity_level_(0),
        k_(0),
        num_stripes_(0),
        next_stripe_(0),
        logical_capacity_(0),
        logical_next_(0) {}
  ~Runtime() override = default;

  /** Fixed per-member stripe (shard) length in bytes. */
  static constexpr chi::u64 kShardLen = 65536;

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

  /** Build/raise parity for dirty stripes (Method::kBuildParity). */
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
  // Per-member runtime bookkeeping: EC state plus the member's reserved
  // contiguous backing region on its bdev pool. NOTE: with declustered
  // placement (Part C) a member's ROLE for a given stripe is computed by the
  // rotation, not stored here — role_/index_ are kept only as a convenience for
  // logging and are NOT used for placement. A member's shard for GLOBAL stripe
  // g lives at base_offset_ + g*kShardLen (slot == global stripe index).
  struct MemberSlot {
    chi::PoolId pool_id_;
    std::string pool_name_;
    chi::u32 node_id_ = 0;
    ec::EcRole role_ = ec::EcRole::kData;
    ec::EcState state_ = ec::EcState::kActive;
    int index_ = -1;            // legacy hint only; placement is rotation-based
    chi::u64 base_offset_ = 0;  // byte offset of stripe 0 on the member pool
  };

  // A generation (epoch) is a frozen (k, m, member-set) config owning a
  // contiguous range of GLOBAL stripe indices, mirroring
  // ec::DeclusteredArray::Generation. Adding a DATA member closes the current
  // generation and opens a wider one (k+1) for FUTURE stripes; existing
  // generations keep their geometry forever (variable width). Within a
  // generation of N = k+m members, the RS shard each member holds rotates by
  // the stripe index (rotated-parity declustering).
  struct Generation {
    int k = 0;
    int m = 0;                  // currently realized parity rows for this gen
    chi::u64 seed = 0;          // rotation seed (varies per generation)
    chi::u64 first_stripe = 0;  // first GLOBAL stripe owned by this gen
    chi::u64 num_stripes = 0;   // 0 while the generation is open (current)
    chi::u64 logical_base = 0;  // logical byte offset of this gen's stripe 0
    std::vector<int> member_pos;  // global member ids (indices into members_)
    std::unique_ptr<ec::ReedSolomon> rs;  // code (k, max_failures_)
  };

  // Client for making calls back to this ChiMod.
  Client client_;

  // EC / membership state.
  std::vector<MemberSlot> members_;  // global member registry
  std::vector<Generation> generations_;  // epochs; last one is the open gen
  chi::u32 max_failures_;            // Fault-tolerance target (M)
  chi::u32 current_parity_level_;    // Parity members added so far (== m grows)
  int k_;                            // k of the CURRENT (open) generation
  chi::u64 num_stripes_;             // Total stripe capacity reserved per member
  chi::u64 next_stripe_;             // Global stripe cursor (next unwritten g)
  chi::u64 logical_capacity_;        // Usable logical bytes across generations
  chi::u64 logical_next_;            // Bump cursor for logical allocation

  // Clients for delegating data-plane I/O to member bdevs. members_ and
  // member_clients_ are kept index-aligned.
  std::vector<clio::run::bdev::Client> member_clients_;

  // Async-write parity bookkeeping. Write writes data shards immediately and
  // records the stripe as dirty (parity not yet current); BuildParity drains
  // the dirty set and (re)computes all current parity rows. written_stripes_
  // tracks every stripe that holds data so a parity-level increase can re-dirty
  // exactly those. Guarded by stripe_mu_ for the brief set operations (never
  // held across a co_await). A stripe is safe to reconstruct only when NOT
  // dirty — degraded reads / recovery refuse a dirty (unprotected) stripe.
  std::set<chi::u64> dirty_stripes_;
  std::set<chi::u64> written_stripes_;
  mutable std::mutex stripe_mu_;

  /** Mark a stripe as holding data and needing (re)parity. */
  void MarkStripeDirty(chi::u64 stripe) {
    std::lock_guard<std::mutex> g(stripe_mu_);
    written_stripes_.insert(stripe);
    dirty_stripes_.insert(stripe);
  }
  /** True if the stripe's parity is not yet current (unprotected). */
  bool IsStripeDirty(chi::u64 stripe) const {
    std::lock_guard<std::mutex> g(stripe_mu_);
    return dirty_stripes_.count(stripe) != 0;
  }

  //==========================================================================
  // EC helpers (defined in safe_bdev_runtime.cc; run inside task fibers).
  //==========================================================================

  /** Per-member pool query (members are independent local bdev pools). */
  chi::PoolQuery MemberQuery() const { return chi::PoolQuery::Local(); }

  //==========================================================================
  // Declustered placement (mirrors ec::DeclusteredArray; see Part C). For a
  // generation of N = k+m members, the RS shard index q played by member
  // position p for global stripe g rotates by (local_stripe + seed) mod N.
  //==========================================================================

  /** Epoch (index into generations_) owning global stripe g, or -1. */
  int EpochOf(chi::u64 g) const {
    for (size_t e = 0; e < generations_.size(); ++e) {
      const Generation &gen = generations_[e];
      const chi::u64 span =
          (gen.num_stripes != 0) ? gen.num_stripes : (next_stripe_ -
                                                      gen.first_stripe);
      if (g >= gen.first_stripe && g < gen.first_stripe + span) {
        return static_cast<int>(e);
      }
    }
    return -1;
  }

  /** RS shard index q held by member position p (in gen) for global stripe g.
   *  q in [0,k) is data column q; q in [k,N) is parity row q-k. */
  int ShardOfPosition(const Generation &gen, chi::u64 g, int p) const {
    const int n = static_cast<int>(gen.member_pos.size());
    const chi::u64 local = g - gen.first_stripe;
    const int r = static_cast<int>((local + gen.seed) % static_cast<chi::u64>(n));
    return ((p - r) % n + n) % n;
  }

  /** Member position holding RS shard q for global stripe g (inverse). */
  int PositionOfShard(const Generation &gen, chi::u64 g, int q) const {
    const int n = static_cast<int>(gen.member_pos.size());
    const chi::u64 local = g - gen.first_stripe;
    const int r = static_cast<int>((local + gen.seed) % static_cast<chi::u64>(n));
    return (q + r) % n;
  }

  /** Global member id (index into members_) holding RS shard q for stripe g. */
  int MemberOfShard(const Generation &gen, chi::u64 g, int q) const {
    return gen.member_pos[static_cast<size_t>(PositionOfShard(gen, g, q))];
  }

  /**
   * Map a logical byte offset L to its (epoch, global stripe g, data column c).
   * The logical space is SEGMENTED per generation: generation e owns the range
   * [gen.logical_base, gen.logical_base + gen.k*kShardLen*span) where span is
   * the number of stripes the generation owns (next_stripe_-first_stripe while
   * open). Returns false if L is out of range. Requires L to be stripe-aligned
   * within its generation for c to be meaningful (callers assert full-stripe
   * alignment in this first-cut milestone).
   */
  bool LogicalToStripe(chi::u64 logical, int *epoch, chi::u64 *g,
                       int *col) const {
    for (size_t e = 0; e < generations_.size(); ++e) {
      const Generation &gen = generations_[e];
      const chi::u64 span =
          (gen.num_stripes != 0) ? gen.num_stripes
                                 : (next_stripe_ - gen.first_stripe);
      const chi::u64 per_stripe = static_cast<chi::u64>(gen.k) * kShardLen;
      const chi::u64 bytes = per_stripe * span;
      if (logical >= gen.logical_base && logical < gen.logical_base + bytes) {
        const chi::u64 within = logical - gen.logical_base;
        *epoch = static_cast<int>(e);
        *g = gen.first_stripe + within / per_stripe;
        *col = static_cast<int>((within % per_stripe) / kShardLen);
        return true;
      }
    }
    return false;
  }

  /** Position of global member id `mid` within gen.member_pos, or -1. */
  int PositionInGen(const Generation &gen, int mid) const {
    for (size_t p = 0; p < gen.member_pos.size(); ++p) {
      if (gen.member_pos[p] == mid) {
        return static_cast<int>(p);
      }
    }
    return -1;
  }

  /** Block list addressing the GLOBAL-stripe `stripe` shard region on member.
   *  Slot == global stripe index: offset = base_offset + stripe*kShardLen. */
  chi::priv::vector<clio::run::bdev::Block> ShardBlocks(const MemberSlot &m,
                                                        chi::u64 stripe) const;

  /**
   * Async write `kShardLen` bytes from host buffer `src` to member `mi`'s
   * shard for `stripe`. Returns false on dispatch failure.
   */
  chi::TaskResume WriteShard(size_t mi, chi::u64 stripe, const uint8_t *src,
                             chi::RunContext &rctx, bool &ok);

  /**
   * Async read member `mi`'s shard for `stripe` into host buffer `dst`.
   */
  chi::TaskResume ReadShard(size_t mi, chi::u64 stripe, uint8_t *dst,
                            chi::RunContext &rctx, bool &ok);

  /**
   * Reconstruct all gen.k data shards for global `stripe` from active
   * survivors (decode), optionally excluding member id `exclude`. `out`
   * receives gen.k buffers. Survivor global shard indices come from the
   * rotated placement (ShardOfPosition).
   */
  chi::TaskResume ReconstructStripe(chi::u64 stripe, int exclude,
                                    chi::RunContext &rctx,
                                    std::vector<std::vector<uint8_t>> &out,
                                    bool &ok);
};

}  // namespace clio::run::safe_bdev

#endif  // SAFE_BDEV_RUNTIME_H_

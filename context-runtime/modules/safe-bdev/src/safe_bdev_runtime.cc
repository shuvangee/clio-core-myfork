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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace clio::run::safe_bdev {

//===========================================================================
// Helpers
//===========================================================================

clio::run::TaskStat Runtime::GetTaskStats(const clio::run::Task *task) const {
  if (task == nullptr) {
    return clio::run::TaskStat();
  }
  switch (task->method_) {
    case Method::kWrite: {
      const auto *wt = static_cast<const WriteTask *>(task);
      clio::run::TaskStat stat;
      stat.io_size_ = wt->length_;
      const size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0F;
      return stat;
    }
    case Method::kRead: {
      const auto *rt = static_cast<const ReadTask *>(task);
      clio::run::TaskStat stat;
      stat.io_size_ = rt->length_;
      const size_t aligned = ((stat.io_size_ + 4095) / 4096) * 4096;
      stat.wall_time_ = static_cast<float>(aligned) / 500.0F;
      return stat;
    }
    default:
      return clio::run::TaskStat();
  }
}

clio::run::priv::vector<clio::run::bdev::Block> Runtime::MemberBlocks(
    clio::run::u64 offset, clio::run::u64 len) const {
  clio::run::priv::vector<clio::run::bdev::Block> blocks(CTP_MALLOC);
  blocks.push_back(clio::run::bdev::Block(offset, len, 0));
  return blocks;
}

void Runtime::OpenGroup(int k, clio::run::u64 first_row, clio::run::u64 num_rows,
                        clio::run::u64 logical_base) {
  auto g = std::make_unique<Group>();
  g->k_ = k;
  g->first_row_ = first_row;
  g->num_rows_ = num_rows;
  g->logical_base_ = logical_base;
  g->logical_span_ = static_cast<clio::run::u64>(k) * num_rows * kChunkLen;
  g->rs_ = std::make_unique<ec::ReedSolomon>(k, static_cast<int>(max_failures_));

  clio::run::WorkOrchestrator *wo = CLIO_WORK_ORCHESTRATOR;
  const size_t num_workers = (wo != nullptr) ? wo->GetWorkerCount() : 16;
  g->block_map_ = std::make_unique<clio::run::bdev::GlobalBlockMap>();
  g->block_map_->Init(num_workers);
  g->heap_ = std::make_unique<clio::run::bdev::Heap>();
  g->heap_->Init(g->logical_span_, /*alignment=*/4096);
  g->allocated_bytes_.store(0);

  groups_.push_back(std::move(g));
}

void Runtime::SeedGroupAllocatorFromLive(
    size_t gi, const std::vector<clio::run::bdev::LiveBlock> &live) {
  // Rebuild group `gi`'s allocator to a state that never re-hands a still-live
  // logical offset. The recovered offsets are GLOBAL logical offsets; the
  // group's heap_/block_map_ operate in the GROUP-LOCAL space [0, span), so
  // convert each: local = global - logical_base_.
  //
  // dev's bdev allocator (Heap/GlobalBlockMap) is a plain bump allocator with a
  // per-worker free list and no live-set seeding hooks, so we reconstruct the
  // one invariant that matters for correctness using only its public API:
  // advance the heap bump past the HIGHEST live end (max local offset + size)
  // via a single throwaway Allocate. After this, Heap::Allocate only ever hands
  // offsets above every live block, and the free list starts empty, so no live
  // offset can be re-handed. Freed gaps that sat BELOW the high-water mark at
  // recovery time are not re-seeded into the free list (a bounded, recovery-only
  // space regression); space from live blocks is reclaimed normally once they
  // are freed via FreeBlocks -> GlobalBlockMap::FreeBlock.
  Group &g = *groups_[gi];

  clio::run::u64 live_bytes = 0;
  clio::run::u64 max_local_end = 0;
  for (const auto &b : live) {
    const clio::run::u64 local_off =
        (b.offset >= g.logical_base_) ? (b.offset - g.logical_base_) : 0;
    const clio::run::u64 end = local_off + b.size;
    if (end > max_local_end) {
      max_local_end = end;
    }
    live_bytes += b.size;
  }

  // Advance the (freshly Init'd) heap bump past every live block with one
  // throwaway carve. The Heap aligns the size up internally, so the resulting
  // bump lands at or above max_local_end; the block is intentionally discarded
  // (never freed) so its span stays reserved.
  if (max_local_end > 0) {
    clio::run::bdev::Block throwaway;
    const int block_type =
        clio::run::bdev::GlobalBlockMap::FindBlockType(max_local_end);
    g.heap_->Allocate(max_local_end,
                      (block_type >= 0) ? block_type : 0, throwaway);
  }

  g.allocated_bytes_.store(live_bytes, std::memory_order_relaxed);
  HLOG(kInfo,
       "safe_bdev: recovered group {} allocator from {} live blocks "
       "(local heap bump past {}, {} live bytes, logical_base={})",
       gi, live.size(), max_local_end, live_bytes, g.logical_base_);
}

//===========================================================================
// Segment I/O helpers (run inside task fibers; co_await member bdev I/O)
//===========================================================================

clio::run::TaskResume Runtime::WriteDataSegment(size_t col, clio::run::u64 offset,
                                          const uint8_t *src, clio::run::u64 len,
                                          bool &ok) {
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(len);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  std::memcpy(buf.ptr_, src, len);
  auto fut = data_clients_[col].AsyncWrite(
      MemberQuery(), MemberBlocks(offset, len),
      buf.shm_.template Cast<void>(), len);
  CLIO_CO_AWAIT(fut);
  ok = (fut->return_code_ == 0) && (fut->bytes_written_ == len);
  MaybeFaultData(col, fut->io_error_);
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::ReadDataSegment(size_t col, clio::run::u64 offset,
                                         uint8_t *dst, clio::run::u64 len,
                                         bool &ok) {
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(len);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  auto fut = data_clients_[col].AsyncRead(
      MemberQuery(), MemberBlocks(offset, len),
      buf.shm_.template Cast<void>(), len);
  CLIO_CO_AWAIT(fut);
  if (fut->return_code_ == 0 && fut->bytes_read_ == len) {
    std::memcpy(dst, buf.ptr_, len);
    ok = true;
  }
  MaybeFaultData(col, fut->io_error_);
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

//===========================================================================
// Member superblock helpers
//===========================================================================

clio::run::TaskResume Runtime::WriteSuperblock(bool is_parity, size_t idx,
                                         bool &ok) {
  CLIO_TASK_BODY_BEGIN
  ok = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kSuperblockSize);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  // Serialize the array identity into a zero-padded superblock buffer.
  std::memset(buf.ptr_, 0, kSuperblockSize);
  const MemberSlot &m =
      is_parity ? parity_members_[idx] : data_members_[idx];
  MemberSuperblock sb;
  sb.magic = kMemberSuperblockMagic;
  sb.format_version = kMemberSuperblockVersion;
  sb.flags = 0;
  sb.array_major = static_cast<uint64_t>(pool_id_.major_);
  sb.array_minor = static_cast<uint64_t>(pool_id_.minor_);
  sb.member_slot = static_cast<uint32_t>(idx);
  sb.role = static_cast<uint32_t>(m.role_);
  sb.index = static_cast<uint32_t>(m.index_);
  sb.max_failures = max_failures_;
  sb.shard_len = kChunkLen;
  sb.epoch = 0;
  sb.checksum = sb.ComputeChecksum();
  std::memcpy(buf.ptr_, &sb, sizeof(sb));

  // The superblock lives at absolute offset 0 on the member.
  auto &client = is_parity ? parity_clients_[idx] : data_clients_[idx];
  auto fut = client.AsyncWrite(MemberQuery(),
                               MemberBlocks(0, kSuperblockSize),
                               buf.shm_.template Cast<void>(), kSuperblockSize);
  CLIO_CO_AWAIT(fut);
  ok = (fut->return_code_ == 0) && (fut->bytes_written_ == kSuperblockSize);
  if (is_parity) {
    MaybeFaultParity(idx, fut->io_error_);
  } else {
    MaybeFaultData(idx, fut->io_error_);
  }
  ipc->FreeBuffer(buf);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::ReadSuperblock(bool is_parity, size_t idx,
                                        MemberSuperblock &sb, bool &present,
                                        bool &ok) {
  CLIO_TASK_BODY_BEGIN
  ok = false;
  present = false;
  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kSuperblockSize);
  if (buf.IsNull()) {
    CLIO_CO_RETURN;
  }
  std::memset(buf.ptr_, 0, kSuperblockSize);
  auto &client = is_parity ? parity_clients_[idx] : data_clients_[idx];
  auto fut = client.AsyncRead(MemberQuery(), MemberBlocks(0, kSuperblockSize),
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
// Row reconstruction (decode a group's k_g data chunks from survivors)
//===========================================================================

clio::run::TaskResume Runtime::ReconstructRow(
    const Group &g, clio::run::u64 row, int exclude_col,
    std::vector<std::vector<uint8_t>> &out, bool &ok) {
  CLIO_TASK_BODY_BEGIN
  ok = false;
  const int kg = g.k_;
  const clio::run::u64 off = ChunkOffset(row);

  // Gather up to k_g active survivors among the group's k_g data + m parity
  // members at this row offset, excluding `exclude_col` (a data column) and any
  // non-active member. A data survivor's global RS shard index is its data
  // column i; a parity survivor's is k_g + j.
  std::vector<int> survivor_index;
  std::vector<std::vector<uint8_t>> survivor_buf;

  // Data survivors (columns [0, k_g)).
  for (int col = 0; col < kg; ++col) {
    if (col == exclude_col) {
      continue;
    }
    if (data_members_[static_cast<size_t>(col)].state_ !=
        ec::EcState::kActive) {
      continue;
    }
    std::vector<uint8_t> buf(kChunkLen, 0);
    bool rd_ok = false;
    CLIO_CO_AWAIT(ReadDataSegment(static_cast<size_t>(col), off, buf.data(),
                                  kChunkLen, rd_ok));
    if (!rd_ok) {
      continue;
    }
    survivor_index.push_back(col);
    survivor_buf.push_back(std::move(buf));
    if (static_cast<int>(survivor_index.size()) == kg) {
      break;
    }
  }

  // Parity survivors (rows [0, m)) if still short.
  for (int j = 0; j < static_cast<int>(parity_level_) &&
                  static_cast<int>(survivor_index.size()) < kg;
       ++j) {
    if (parity_members_[static_cast<size_t>(j)].state_ !=
        ec::EcState::kActive) {
      continue;
    }
    std::vector<uint8_t> buf(kChunkLen, 0);
    bool rd_ok = false;
    auto *ipc = CLIO_IPC;
    ctp::ipc::FullPtr<char> rbuf = ipc->AllocateBuffer(kChunkLen);
    if (rbuf.IsNull()) {
      CLIO_CO_RETURN;
    }
    auto fut = parity_clients_[static_cast<size_t>(j)].AsyncRead(
        MemberQuery(), MemberBlocks(off, kChunkLen),
        rbuf.shm_.template Cast<void>(), kChunkLen);
    CLIO_CO_AWAIT(fut);
    rd_ok = (fut->return_code_ == 0) && (fut->bytes_read_ == kChunkLen);
    if (rd_ok) {
      std::memcpy(buf.data(), rbuf.ptr_, kChunkLen);
    }
    MaybeFaultParity(static_cast<size_t>(j), fut->io_error_);
    ipc->FreeBuffer(rbuf);
    if (!rd_ok) {
      continue;
    }
    survivor_index.push_back(kg + j);
    survivor_buf.push_back(std::move(buf));
  }

  if (static_cast<int>(survivor_index.size()) < kg) {
    CLIO_CO_RETURN;  // Too many failures to reconstruct.
  }
  std::vector<const uint8_t *> ptrs(survivor_buf.size());
  for (size_t i = 0; i < survivor_buf.size(); ++i) {
    ptrs[i] = survivor_buf[i].data();
  }
  ok = g.rs_->DecodeData(survivor_index, ptrs, kChunkLen, &out);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::ReconstructDataChunk(const Group &g, clio::run::u64 row,
                                              int data_col, int exclude_col,
                                              std::vector<uint8_t> &out,
                                              bool &ok) {
  CLIO_TASK_BODY_BEGIN
  ok = false;
  std::vector<std::vector<uint8_t>> data_chunks;
  bool rec_ok = false;
  CLIO_CO_AWAIT(ReconstructRow(g, row, exclude_col, data_chunks, rec_ok));
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

// The member manifest uses a small hand-rolled binary format (NOT msgpack) so
// the runtime .so pulls in no msgpack UNPACK symbol -- keeping its dynamic
// symbol table clean for the modules that dlopen it. Layout (native-endian
// u32; same-host restart):
//   [magic][count]  then per member:
//   [role][index][pool_major][pool_minor][node_id][state][recovering]
//   [name_len][name bytes...]
namespace {
constexpr clio::run::u32 kMemberManifestMagic = 0x53424d31;  // "SBM1"
}  // namespace

void Runtime::PersistMemberManifest() const {
  if (members_manifest_path_.empty()) {
    return;
  }
  const std::string tmp = members_manifest_path_ + ".tmp";
  std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
  if (!ofs.is_open()) {
    HLOG(kWarning, "safe_bdev: cannot open member manifest tmp '{}'", tmp);
    return;
  }
  const auto put_u32 = [&](clio::run::u32 v) {
    ofs.write(reinterpret_cast<const char *>(&v), sizeof(v));
  };
  const auto put_member = [&](const MemberSlot &m, clio::run::u32 role) {
    put_u32(role);
    put_u32(static_cast<clio::run::u32>(m.index_));
    put_u32(m.pool_id_.major_);
    put_u32(m.pool_id_.minor_);
    put_u32(m.node_id_);
    put_u32(static_cast<clio::run::u32>(m.state_));
    put_u32(m.recovering_ ? 1u : 0u);
    put_u32(static_cast<clio::run::u32>(m.pool_name_.size()));
    if (!m.pool_name_.empty()) {
      ofs.write(m.pool_name_.data(),
                static_cast<std::streamsize>(m.pool_name_.size()));
    }
  };
  put_u32(kMemberManifestMagic);
  put_u32(static_cast<clio::run::u32>(data_members_.size() +
                                      parity_members_.size()));
  for (const auto &m : data_members_) put_member(m, 0);
  for (const auto &m : parity_members_) put_member(m, 1);
  ofs.flush();
  ofs.close();
  // Atomic replace.
  if (std::rename(tmp.c_str(), members_manifest_path_.c_str()) != 0) {
    HLOG(kWarning, "safe_bdev: member manifest rename '{}' -> '{}' failed", tmp,
         members_manifest_path_);
  }
}

bool Runtime::LoadMemberManifest(std::vector<MemberManifestEntry> &out) const {
  out.clear();
  if (members_manifest_path_.empty()) {
    return false;
  }
  std::ifstream ifs(members_manifest_path_, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }
  const auto get_u32 = [&](clio::run::u32 &v) -> bool {
    return static_cast<bool>(
        ifs.read(reinterpret_cast<char *>(&v), sizeof(v)));
  };
  clio::run::u32 magic = 0;
  clio::run::u32 count = 0;
  if (!get_u32(magic) || magic != kMemberManifestMagic || !get_u32(count)) {
    return false;
  }
  for (clio::run::u32 i = 0; i < count; ++i) {
    MemberManifestEntry e;
    clio::run::u32 name_len = 0;
    if (!get_u32(e.role_) || !get_u32(e.index_) || !get_u32(e.pool_major_) ||
        !get_u32(e.pool_minor_) || !get_u32(e.node_id_) || !get_u32(e.state_) ||
        !get_u32(e.recovering_) || !get_u32(name_len)) {
      out.clear();
      return false;
    }
    if (name_len > (1u << 20)) {  // sanity bound
      out.clear();
      return false;
    }
    e.pool_name_.resize(name_len);
    if (name_len > 0 &&
        !ifs.read(&e.pool_name_[0], static_cast<std::streamsize>(name_len))) {
      out.clear();
      return false;
    }
    out.push_back(std::move(e));
  }
  return !out.empty();
}

clio::run::TaskResume Runtime::Create(clio::run::shared_ptr<CreateTask> &task) {
  CLIO_TASK_BODY_BEGIN

  // Get the creation parameters.
  CreateParams params = task->GetParams();

  max_failures_ = params.max_failures_;
  parity_level_ = 0;
  data_members_.clear();
  parity_members_.clear();
  data_clients_.clear();
  parity_clients_.clear();
  groups_.clear();
  total_rows_ = 0;
  max_phys_rows_ = 0;
  reattached_members_ = 0;
  {
    std::lock_guard<std::mutex> g(row_mu_);
    dirty_rows_.clear();
    written_rows_.clear();
  }

  // DATA members come from the config member list and are re-attached by
  // SUPERBLOCK identity (pool ids may change across restart -- the safe-bdev
  // recognizes its own members by their superblocks, not their pool ids). The
  // durable manifest augments this with what the config CANNOT express: the
  // runtime-added PARITY members, and the per-member RECOVERY state so an
  // interrupted recovery resumes. On a fresh create there is no manifest yet.
  members_manifest_path_ =
      params.alloc_log_path_.empty() ? std::string()
                                     : (params.alloc_log_path_ + ".members");
  std::vector<MemberManifestEntry> manifest;
  const bool have_manifest = LoadMemberManifest(manifest);

  std::vector<MemberManifestEntry> eff_parity;  // parity members from manifest
  // data_recovering[col] == true => that data column is a recovery replacement
  // whose rebuild must be resumed. Indexed by data column.
  std::vector<bool> data_recovering(params.members_.size(), false);
  if (have_manifest) {
    for (const auto &e : manifest) {
      if (e.role_ == 1) {
        eff_parity.push_back(e);
      } else if (e.recovering_ != 0 && e.index_ < data_recovering.size()) {
        data_recovering[e.index_] = true;
      }
    }
    std::sort(eff_parity.begin(), eff_parity.end(),
              [](const MemberManifestEntry &a, const MemberManifestEntry &b) {
                return a.index_ < b.index_;
              });
    HLOG(kInfo,
         "safe_bdev Create: manifest '{}' augments membership with {} parity "
         "member(s); {} data column(s) mid-recovery",
         members_manifest_path_, eff_parity.size(),
         std::count(data_recovering.begin(), data_recovering.end(), true));
  }

  // Initial members are DATA members; k_0 = number supplied.
  const int k0 = static_cast<int>(params.members_.size());
  if (k0 <= 0) {
    HLOG(kError, "safe_bdev Create: no member bdevs supplied");
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  // Query the smallest member's remaining capacity. usable_per_member is the
  // largest multiple of kChunkLen that fits after the superblock; max_phys_rows
  // is that many chunks. Group 0 spans ALL of these rows, so a never-grown array
  // uses the full member capacity; an added data drive later freezes the current
  // group at its high-water mark and the new group takes the remaining rows.
  clio::run::u64 min_remaining = ~static_cast<clio::run::u64>(0);
  for (const auto &desc : params.members_) {
    data_clients_.emplace_back(desc.pool_id_);
    auto stats = data_clients_.back().AsyncGetStats();
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
  const clio::run::u64 avail =
      (min_remaining > kSuperblockSize) ? (min_remaining - kSuperblockSize) : 0;
  const clio::run::u64 usable_per_member = (avail / kChunkLen) * kChunkLen;
  max_phys_rows_ = usable_per_member / kChunkLen;
  if (max_phys_rows_ == 0) {
    HLOG(kError, "safe_bdev Create: member too small for even one stripe row");
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  // Group 0 spans ALL physical rows (full capacity); later add-data groups carve
  // from whatever the current group hasn't filled.
  const clio::run::u64 num_rows0 = max_phys_rows_;

  // Open the persistent allocator-state log (WAL). Empty path => disabled (no
  // file created). On recover, replay it so we can reconstruct the append-only
  // group structure and each group's allocator without re-handing-out any
  // still-live logical offset. REUSED from the bdev module.
  bool recovered_groups = false;
  if (!alloc_log_.Open(params.alloc_log_path_, /*recover=*/true)) {
    HLOG(kWarning,
         "safe_bdev Create: failed to open alloc log at '{}', logging disabled",
         params.alloc_log_path_);
  }
  const std::vector<clio::run::bdev::GroupRec> recovered_grps =
      alloc_log_.enabled() ? alloc_log_.groups()
                           : std::vector<clio::run::bdev::GroupRec>{};
  recovered_groups = !recovered_grps.empty();

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
    data_members_.push_back(slot);

    MemberSuperblock sb;
    bool present = false;
    bool sb_ok = false;
    CLIO_CO_AWAIT(ReadSuperblock(/*is_parity=*/false, static_cast<size_t>(col),
                                 sb, present, sb_ok));
    if (!sb_ok) {
      HLOG(kError, "safe_bdev Create: superblock read failed for member '{}'",
           desc.pool_name_);
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    if (!present) {
      bool wr_ok = false;
      CLIO_CO_AWAIT(WriteSuperblock(/*is_parity=*/false,
                                    static_cast<size_t>(col), wr_ok));
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

  // A data member restored from the manifest as mid-recovery stays non-active
  // (kFaulty + recovering) so degraded reads serve its data until ResumeRecov-
  // eries() finishes the rebuild below.
  for (size_t i = 0; i < data_members_.size(); ++i) {
    if (i < data_recovering.size() && data_recovering[i]) {
      data_members_[i].state_ = ec::EcState::kFaulty;
      data_members_[i].recovering_ = true;
    }
  }

  if (recovered_groups) {
    // RECOVERY: reconstruct the append-only group structure from the recovered
    // GroupRecs (in group_id order; group_id == group INDEX) using the recovered
    // geometry verbatim (do NOT recompute sizes). Then seed each group's
    // allocator from its recovered live set. total_rows_/max_phys_rows_ stay as
    // computed from member capacity above (the physical row count is a property
    // of the members, unchanged across restart).
    total_rows_ = max_phys_rows_;
    std::vector<clio::run::bdev::GroupRec> ordered = recovered_grps;
    std::sort(ordered.begin(), ordered.end(),
              [](const clio::run::bdev::GroupRec &a,
                 const clio::run::bdev::GroupRec &b) {
                return a.group_id < b.group_id;
              });
    for (const auto &gr : ordered) {
      OpenGroup(static_cast<int>(gr.k), gr.first_row, gr.num_rows,
                gr.logical_base);
      const size_t gi = groups_.size() - 1;
      const std::vector<clio::run::bdev::LiveBlock> &live =
          alloc_log_.live(static_cast<clio::run::u32>(gr.group_id));
      SeedGroupAllocatorFromLive(gi, live);
    }
    HLOG(kInfo,
         "safe_bdev Create: RECOVERED {} group(s) from alloc log '{}' "
         "(widest k={}, max_phys_rows={})",
         groups_.size(), params.alloc_log_path_,
         groups_.empty() ? 0 : groups_.back()->k_, max_phys_rows_);
  } else {
    // FRESH: open group 0 spanning rows [0, num_rows0) over k0 data drives,
    // logical base 0. Its rs_ / allocator are constructed by OpenGroup. Log the
    // group-open so a later restart can recover the geometry.
    OpenGroup(k0, /*first_row=*/0, num_rows0, /*logical_base=*/0);
    total_rows_ = num_rows0;
    alloc_log_.LogGroupOpen(/*group_id=*/0, static_cast<clio::run::u32>(k0),
                            /*first_row=*/0, num_rows0, /*logical_base=*/0);

    HLOG(kInfo,
         "safe_bdev Create: pool='{}', k0={}, M={}, num_rows0={}, "
         "logical_span0={} (append-only RAID-0 groups + dedicated parity)",
         task->pool_name_.str(), k0, max_failures_, num_rows0,
         groups_[0]->logical_span_);
  }

  // Restore PARITY members from the manifest. Config carries only data members,
  // so eff_parity is empty on a fresh create (parity is added later via
  // AddBdev); on restart the manifest supplies the runtime-added parity drives.
  // Each manifest parity pool is probed by superblock before adoption:
  //   - mid-recovery (blank replacement)  -> adopt + mark recovering (rebuilt
  //     by ResumeRecoveries below),
  //   - superblock present AND ours       -> adopt (re-attach),
  //   - otherwise (unreachable / foreign) -> SKIP: a stale manifest entry (the
  //     pool was re-composed under a different id and will be re-added).
  for (const auto &pe : eff_parity) {
    MemberSlot slot;
    slot.pool_id_ = clio::run::PoolId(pe.pool_major_, pe.pool_minor_);
    slot.pool_name_ = pe.pool_name_;
    slot.node_id_ = pe.node_id_;
    slot.role_ = ec::EcRole::kParity;
    slot.index_ = static_cast<int>(pe.index_);
    const bool par_recovering = (pe.recovering_ != 0);

    // Probe the candidate on a temporary client BEFORE committing it to the
    // parity vectors (so a skip leaves no dead client behind).
    parity_clients_.emplace_back(slot.pool_id_);
    parity_members_.push_back(slot);
    const size_t pj = parity_members_.size() - 1;
    MemberSuperblock sb;
    bool present = false;
    bool sb_ok = false;
    CLIO_CO_AWAIT(ReadSuperblock(/*is_parity=*/true, pj, sb, present, sb_ok));
    const bool ours = sb_ok && present &&
                      sb.array_major == static_cast<uint64_t>(pool_id_.major_) &&
                      sb.array_minor == static_cast<uint64_t>(pool_id_.minor_);
    if (par_recovering) {
      parity_members_[pj].state_ = ec::EcState::kFaulty;  // non-active
      parity_members_[pj].recovering_ = true;
    } else if (ours) {
      parity_members_[pj].state_ = ec::EcState::kActive;
      parity_members_[pj].recovering_ = false;
    } else {
      // Stale/unreachable -> drop it.
      parity_members_.pop_back();
      parity_clients_.pop_back();
      HLOG(kInfo,
           "safe_bdev Create: skipping stale manifest parity member "
           "(pool {}.{} not reachable/ours)",
           pe.pool_major_, pe.pool_minor_);
    }
  }
  parity_level_ = static_cast<clio::run::u32>(parity_members_.size());

  // Register a periodic task that flushes (and compacts) the WAL. Only when
  // logging is enabled. Mirrors bdev's SetPeriod + TASK_PERIODIC pattern.
  if (alloc_log_.enabled()) {
    client_.AsyncFlushAllocLog(MemberQuery(), kFlushAllocLogPeriodUs);
  }

  // Kick off the background parity builder. Write records dirty rows off the
  // critical path; this periodic task converges them to full protection.
  client_.AsyncBuildParity(MemberQuery(), /*max_batch=*/0,
                           /*period_us=*/kBuildParityPeriodUs);

  // Resume any recovery interrupted by a crash (members restored as recovering),
  // then persist the current membership so the NEXT restart is consistent (this
  // also writes the initial manifest on a fresh create).
  CLIO_CO_AWAIT(ResumeRecoveries());
  PersistMemberManifest();

  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::AllocateBlocks(
    clio::run::shared_ptr<AllocateBlocksTask> &task) {
  CLIO_TASK_BODY_BEGIN
  // Real reclaimable allocation from the CURRENT (widest) group: try the free
  // list first (reuse a freed block), else carve from the heap. Offsets are
  // shifted into the group's global logical range by adding logical_base_.
  const clio::run::u64 size = task->size_;
  if (size == 0) {
    task->blocks_.clear();
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }
  if (groups_.empty()) {
    task->blocks_.clear();
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  Group &g = CurrentGroup();
  clio::run::Worker *worker = CLIO_CUR_WORKER;
  const int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId())
                                            : 0;

  clio::run::bdev::Block block;
  bool allocated = g.block_map_->AllocateBlock(worker_id, size, block);
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
    allocated = g.heap_->Allocate(size, block_type, block);
    if (allocated) {
      // Heap returns a group-local offset; shift into the global logical range.
      block.offset_ += g.logical_base_;
    }
  }
  if (!allocated) {
    HLOG(kError,
         "safe_bdev AllocateBlocks: current group full ({} bytes requested)",
         size);
    task->blocks_.clear();
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  g.allocated_bytes_.fetch_add(block.size_, std::memory_order_relaxed);
  // Persist the allocation under the CURRENT group's id (== its index in
  // groups_). block.offset_ is the GLOBAL logical offset.
  const clio::run::u32 gi = static_cast<clio::run::u32>(groups_.size() - 1);
  alloc_log_.LogAlloc(gi, block.offset_, block.size_, block.block_type_);
  task->blocks_.clear();
  task->blocks_.push_back(block);
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::FreeBlocks(clio::run::shared_ptr<FreeBlocksTask> &task) {
  CLIO_TASK_BODY_BEGIN
  // Route each block to its OWNING group (by logical offset) and return it to
  // that group's free list (real reclaim). Normalize block_type_ from the block
  // SIZE so AllocateBlock (which picks the free list by size class) can find it
  // again — mirrors bdev::FreeBlocks.
  clio::run::Worker *worker = CLIO_CUR_WORKER;
  const int worker_id = (worker != nullptr) ? static_cast<int>(worker->GetId())
                                            : 0;
  const size_t buckets[] = {4096, 16384, 32768, 65536, 131072, 1048576};
  for (size_t i = 0; i < task->blocks_.size(); ++i) {
    clio::run::bdev::Block b = task->blocks_[i];
    const int gi = FindGroupByOffset(b.offset_);
    if (gi < 0) {
      HLOG(kWarning,
           "safe_bdev FreeBlocks: block at logical offset {} not owned by any "
           "group; skipping",
           b.offset_);
      continue;
    }
    int bt = 5;
    for (size_t j = 0; j < 6; ++j) {
      if (buckets[j] >= static_cast<size_t>(b.size_)) {
        bt = static_cast<int>(j);
        break;
      }
    }
    b.block_type_ = static_cast<clio::run::u32>(bt);
    Group &g = *groups_[static_cast<size_t>(gi)];
    g.block_map_->FreeBlock(worker_id, b);
    const clio::run::u64 cur = g.allocated_bytes_.load(std::memory_order_relaxed);
    g.allocated_bytes_.store(cur >= b.size_ ? cur - b.size_ : 0,
                             std::memory_order_relaxed);
    // Persist the free under the owning group's id (== its index in groups_).
    // b.offset_ is the GLOBAL logical offset; the WAL drops the matching live
    // alloc by (group_id, offset) on recovery.
    alloc_log_.LogFree(static_cast<clio::run::u32>(gi), b.offset_, b.size_,
                       b.block_type_);
  }
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Write(clio::run::shared_ptr<WriteTask> &task) {
  CLIO_TASK_BODY_BEGIN
  if (groups_.empty() || task->blocks_.size() == 0) {
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> data =
      ipc->ToFullPtr(task->data_).template Cast<char>();

  // The data buffer fills the allocated blocks in order. For each block, find
  // its owning group, then walk its CHUNK-segments: a segment is the block's
  // intersection with one RAID-0 chunk inside that group. It maps (under the
  // group's k_g) to DATA member data_col at physical offset
  // kSuperblockSize + global_row*kChunkLen + within. Segments across data
  // members are dispatched in parallel. Parity is deferred (global rows marked
  // dirty).
  clio::run::u64 buf_pos = 0;  // running offset into the host data buffer
  clio::run::u64 bytes_written = 0;
  std::set<clio::run::u64> touched_rows;

  for (size_t bi = 0; bi < task->blocks_.size(); ++bi) {
    const clio::run::u64 lo = task->blocks_[bi].offset_;
    const clio::run::u64 ls = task->blocks_[bi].size_;
    const clio::run::u64 hi = lo + ls;

    const int gi = FindGroupByOffset(lo);
    if (gi < 0) {
      HLOG(kError, "safe_bdev Write: block at logical offset {} not in any "
           "group", lo);
      task->bytes_written_ = bytes_written;
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    const Group &g = *groups_[static_cast<size_t>(gi)];
    const clio::run::u64 kg = static_cast<clio::run::u64>(g.k_);

    std::vector<clio::run::Future<WriteTask>> futs;
    std::vector<ctp::ipc::FullPtr<char>> bufs;
    bool dispatch_ok = true;

    clio::run::u64 cur = lo;
    while (cur < hi && dispatch_ok) {
      const clio::run::u64 local = cur - g.logical_base_;
      const clio::run::u64 ci = local / kChunkLen;          // chunk within group
      const clio::run::u64 chunk_end =
          g.logical_base_ + (ci + 1) * kChunkLen;
      const clio::run::u64 seg_end = std::min(hi, chunk_end);
      const clio::run::u64 seg_len = seg_end - cur;
      const int data_col = static_cast<int>(ci % kg);
      const clio::run::u64 row_in_grp = ci / kg;
      const clio::run::u64 global_row = g.first_row_ + row_in_grp;
      const clio::run::u64 within = local % kChunkLen;
      const clio::run::u64 phys = ChunkOffset(global_row) + within;

      MemberSlot &m = data_members_[static_cast<size_t>(data_col)];
      if (m.state_ == ec::EcState::kActive) {
        ctp::ipc::FullPtr<char> seg = ipc->AllocateBuffer(seg_len);
        if (seg.IsNull()) {
          dispatch_ok = false;
          break;
        }
        std::memcpy(seg.ptr_, data.ptr_ + buf_pos, seg_len);
        bufs.push_back(seg);
        futs.push_back(data_clients_[static_cast<size_t>(data_col)].AsyncWrite(
            MemberQuery(), MemberBlocks(phys, seg_len),
            seg.shm_.template Cast<void>(), seg_len));
      }
      touched_rows.insert(global_row);
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

  // Mark all global rows the write touched dirty; BuildParity (re)computes
  // parity under each row's owning group's code.
  for (clio::run::u64 r : touched_rows) {
    MarkRowDirty(r);
  }

  task->bytes_written_ = bytes_written;
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Read(clio::run::shared_ptr<ReadTask> &task) {
  CLIO_TASK_BODY_BEGIN
  if (groups_.empty() || task->blocks_.size() == 0) {
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  auto *ipc = CLIO_IPC;
  ctp::ipc::FullPtr<char> data =
      ipc->ToFullPtr(task->data_).template Cast<char>();

  // Symmetric decomposition of Write: find each block's group, walk its
  // chunk-segments under that group's k_g. If the segment's DATA member is
  // active, AsyncRead it directly; if faulty, reconstruct that member's FULL
  // chunk for the global row (guarding dirty rows) under the group's code and
  // copy the needed within-slice.
  clio::run::u64 buf_pos = 0;
  clio::run::u64 bytes_read = 0;

  for (size_t bi = 0; bi < task->blocks_.size(); ++bi) {
    const clio::run::u64 lo = task->blocks_[bi].offset_;
    const clio::run::u64 ls = task->blocks_[bi].size_;
    const clio::run::u64 hi = lo + ls;

    const int gi = FindGroupByOffset(lo);
    if (gi < 0) {
      HLOG(kError, "safe_bdev Read: block at logical offset {} not in any "
           "group", lo);
      task->bytes_read_ = bytes_read;
      task->length_ = bytes_read;
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    const Group &g = *groups_[static_cast<size_t>(gi)];
    const clio::run::u64 kg = static_cast<clio::run::u64>(g.k_);

    clio::run::u64 cur = lo;
    while (cur < hi) {
      const clio::run::u64 local = cur - g.logical_base_;
      const clio::run::u64 ci = local / kChunkLen;
      const clio::run::u64 chunk_end = g.logical_base_ + (ci + 1) * kChunkLen;
      const clio::run::u64 seg_end = std::min(hi, chunk_end);
      const clio::run::u64 seg_len = seg_end - cur;
      const int data_col = static_cast<int>(ci % kg);
      const clio::run::u64 row_in_grp = ci / kg;
      const clio::run::u64 global_row = g.first_row_ + row_in_grp;
      const clio::run::u64 within = local % kChunkLen;
      const clio::run::u64 phys = ChunkOffset(global_row) + within;

      const MemberSlot &m = data_members_[static_cast<size_t>(data_col)];
      bool seg_ok = false;
      if (m.state_ == ec::EcState::kActive) {
        CLIO_CO_AWAIT(ReadDataSegment(static_cast<size_t>(data_col), phys,
                                      reinterpret_cast<uint8_t *>(data.ptr_) +
                                          buf_pos,
                                      seg_len, seg_ok));
      } else if (IsRowDirty(global_row)) {
        // A data member is down AND this row's parity is not current: it is
        // unprotected, so reconstruction would yield wrong bytes. Fail loudly.
        HLOG(kError,
             "safe_bdev Read: global row {} is dirty (parity not built) and "
             "data member {} is down — cannot reconstruct",
             global_row, data_col);
        seg_ok = false;
      } else {
        std::vector<uint8_t> chunk;
        CLIO_CO_AWAIT(ReconstructDataChunk(g, global_row, data_col,
                                           /*exclude_col=*/-1, chunk,
                                           seg_ok));
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

clio::run::TaskResume Runtime::GetStats(clio::run::shared_ptr<GetStatsTask> &task) {
  CLIO_TASK_BODY_BEGIN
  // Only the current (open) group is allocatable; frozen groups no longer hand
  // out new blocks, so their leftover space is not counted as usable remaining.
  clio::run::u64 remaining = 0;
  if (!groups_.empty()) {
    const Group &g = *groups_.back();
    const clio::run::u64 used = g.allocated_bytes_.load(std::memory_order_relaxed);
    remaining = (g.logical_span_ > used) ? (g.logical_span_ - used) : 0;
  }
  task->remaining_size_ = remaining;
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::AddBdev(clio::run::shared_ptr<AddBdevTask> &task) {
  CLIO_TASK_BODY_BEGIN

  const bool as_parity = (task->as_parity_ != 0);

  if (!as_parity) {
    // --- Add a DATA member via an append-only RAID-0 group, NO reshuffle. ---
    // Freeze the current (widest) group at its HIGH-WATER mark — the rows it has
    // actually bump-allocated — and open a new wider group over the remaining
    // physical rows. Existing groups + their data + parity are untouched. The
    // current group's heap bump only advances (frees go to the per-group free
    // list, not the heap), so logical_span_ - GetRemainingSize() is its
    // high-water in logical bytes.
    const Group &cur = CurrentGroup();
    const clio::run::u64 remaining_bytes = cur.heap_->GetRemainingSize();
    const clio::run::u64 bump_used = (cur.logical_span_ > remaining_bytes)
                                   ? (cur.logical_span_ - remaining_bytes)
                                   : 0;
    const clio::run::u64 per_row = static_cast<clio::run::u64>(cur.k_) * kChunkLen;
    const clio::run::u64 used_rows = (bump_used + per_row - 1) / per_row;  // ceil
    const clio::run::u64 new_first_row = cur.first_row_ + used_rows;
    if (groups_.size() >= kMaxGroups || new_first_row >= max_phys_rows_) {
      HLOG(kError,
           "safe_bdev AddBdev(as_data): no room for a new group (groups={}, "
           "new_first_row={}, max_phys_rows={}); member '{}' not added",
           groups_.size(), new_first_row, max_phys_rows_,
           task->pool_name_.str());
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    data_clients_.emplace_back(task->member_pool_id_);

    // Confirm the new member is usable: GetStats, then classify its superblock
    // (fresh / ours / foreign) exactly like Create does for data members.
    auto stats = data_clients_.back().AsyncGetStats();
    CLIO_CO_AWAIT(stats);
    if (stats->return_code_ != 0) {
      HLOG(kError, "safe_bdev AddBdev: GetStats failed for data member '{}'",
           task->pool_name_.str());
      data_clients_.pop_back();
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }

    const int new_col = static_cast<int>(data_members_.size());
    MemberSlot slot;
    slot.pool_id_ = task->member_pool_id_;
    slot.pool_name_ = task->pool_name_.str();
    slot.node_id_ = task->node_id_;
    slot.role_ = ec::EcRole::kData;
    slot.state_ = ec::EcState::kActive;
    slot.index_ = new_col;
    data_members_.push_back(slot);

    MemberSuperblock sb;
    bool present = false;
    bool sb_ok = false;
    CLIO_CO_AWAIT(ReadSuperblock(/*is_parity=*/false,
                                 static_cast<size_t>(new_col), sb,
                                 present, sb_ok));
    if (!sb_ok) {
      HLOG(kError, "safe_bdev AddBdev: superblock read failed for member '{}'",
           task->pool_name_.str());
      data_members_.pop_back();
      data_clients_.pop_back();
      task->return_code_ = 1;
      CLIO_CO_RETURN;
    }
    if (present && (sb.array_major != static_cast<uint64_t>(pool_id_.major_) ||
                    sb.array_minor != static_cast<uint64_t>(pool_id_.minor_))) {
      HLOG(kError,
           "safe_bdev AddBdev: REFUSING data member '{}' — owned by FOREIGN "
           "array ({},{})",
           task->pool_name_.str(), sb.array_major, sb.array_minor);
      data_members_.pop_back();
      data_clients_.pop_back();
      task->return_code_ = 2;
      CLIO_CO_RETURN;
    }
    if (!present) {
      bool wr_ok = false;
      CLIO_CO_AWAIT(WriteSuperblock(/*is_parity=*/false,
                                    static_cast<size_t>(new_col), wr_ok));
      if (!wr_ok) {
        HLOG(kWarning,
             "safe_bdev AddBdev: superblock write failed for new data member "
             "'{}' (seated; will be re-stamped on next attach)",
             task->pool_name_.str());
      }
    }

    // Freeze the current group at its high-water (used_rows) so the new group
    // can take the physical rows above it; every block already allocated in the
    // current group sits below that cut, so nothing moves.
    Group &prev = CurrentGroup();
    prev.num_rows_ = used_rows;
    prev.logical_span_ = static_cast<clio::run::u64>(prev.k_) * used_rows * kChunkLen;

    // Open a new group of width (data_members_.size()) over ALL remaining rows
    // [new_first_row, max_phys_rows_); logical_base continues after the frozen
    // group's (now shrunk) logical range.
    const int new_k = static_cast<int>(data_members_.size());
    const clio::run::u64 new_num_rows = max_phys_rows_ - new_first_row;
    const clio::run::u64 logical_base = prev.LogicalEnd();

    // Persist the group transition: freeze the previous group at its high-water
    // (used_rows) and open the new wider group. group_id == group INDEX in
    // groups_. The freeze records the previous group's index (size()-1 BEFORE
    // the new group is pushed); the open records the new group's index.
    const clio::run::u32 prev_gi = static_cast<clio::run::u32>(groups_.size() - 1);
    alloc_log_.LogGroupFreeze(prev_gi, used_rows);
    OpenGroup(new_k, new_first_row, new_num_rows, logical_base);
    const clio::run::u32 new_gi = static_cast<clio::run::u32>(groups_.size() - 1);
    alloc_log_.LogGroupOpen(new_gi, static_cast<clio::run::u32>(new_k), new_first_row,
                            new_num_rows, logical_base);

    HLOG(kInfo,
         "safe_bdev AddBdev(as_data): froze group {} at {} rows; opened group {} "
         "(k={}, first_row={}, num_rows={}); existing data untouched (no "
         "reshuffle)",
         groups_.size() - 2, used_rows, groups_.size() - 1, new_k, new_first_row,
         new_num_rows);

    PersistMemberManifest();  // durable: new data member is part of the array
    task->return_code_ = 0;
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

  // Append a PARITY member. Its parity row j == parity_level_; per global row
  // it carries that row's group's RS shard index k_g + j. Parity computation is
  // deferred: re-dirty every written row so BuildParity computes the new parity
  // column off the critical path (each row uses its own group's rs_g). The
  // incremental RS property means each parity row is an independent function of
  // the data, so existing parity is never read or rewritten.
  parity_clients_.emplace_back(task->member_pool_id_);
  MemberSlot slot;
  slot.pool_id_ = task->member_pool_id_;
  slot.pool_name_ = task->pool_name_.str();
  slot.node_id_ = task->node_id_;
  slot.role_ = ec::EcRole::kParity;
  slot.state_ = ec::EcState::kActive;
  slot.index_ = static_cast<int>(parity_level_);
  const size_t new_j = parity_members_.size();
  parity_members_.push_back(slot);
  ++parity_level_;

  {
    std::lock_guard<std::mutex> g(row_mu_);
    for (clio::run::u64 r : written_rows_) {
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
    CLIO_CO_AWAIT(WriteSuperblock(/*is_parity=*/true, new_j, wr_ok));
    if (!wr_ok) {
      HLOG(kWarning,
           "safe_bdev AddBdev: superblock write failed for new parity member "
           "'{}' (member seated; will be re-stamped on next attach)",
           task->pool_name_.str());
    }
  }

  PersistMemberManifest();  // durable: new parity member is part of the array
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::RemoveBdev(clio::run::shared_ptr<RemoveBdevTask> &task) {
  CLIO_TASK_BODY_BEGIN
  // Search data then parity members by pool id; mark faulty (recovery
  // candidate) or removed (clean unlink). State is positional — we never erase
  // the slot.
  const auto apply = [&](MemberSlot &m, const char *kind, size_t idx) {
    if (task->was_faulty_ != 0) {
      m.state_ = ec::EcState::kFaulty;
      HLOG(kInfo, "safe_bdev RemoveBdev: {} member {} marked faulty", kind, idx);
    } else {
      m.state_ = ec::EcState::kRemoved;
      HLOG(kInfo, "safe_bdev RemoveBdev: {} member {} unlinked (marked removed)",
           kind, idx);
    }
  };
  bool found = false;
  for (size_t i = 0; i < data_members_.size() && !found; ++i) {
    if (data_members_[i].pool_id_ == task->target_pool_id_) {
      apply(data_members_[i], "data", i);
      found = true;
    }
  }
  for (size_t i = 0; i < parity_members_.size() && !found; ++i) {
    if (parity_members_[i].pool_id_ == task->target_pool_id_) {
      apply(parity_members_[i], "parity", i);
      found = true;
    }
  }
  if (found) {
    PersistMemberManifest();  // durable: the member is now faulty/removed
  }
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::RebuildMember(bool is_data, int idx, bool &ok,
                                             bool &completed) {
  CLIO_TASK_BODY_BEGIN
  ok = true;
  completed = true;
  auto &client = is_data ? data_clients_[static_cast<size_t>(idx)]
                         : parity_clients_[static_cast<size_t>(idx)];
  auto *ipc = CLIO_IPC;

  // Test hook: stop after this many rows to simulate a recovery interrupted by
  // a crash (0 / unset => rebuild everything). The manifest keeps the member
  // marked recovering, so the next Create() resumes it.
  clio::run::u64 max_rows = 0;
  if (const char *env = std::getenv("CLIO_SAFE_BDEV_RECOVER_MAX_ROWS")) {
    max_rows = std::strtoull(env, nullptr, 10);
  }

  // Count the rows this member participates in up front (a DATA column d only
  // exists in groups with k_g > d; a PARITY member exists in every group) and
  // publish it for the recovery dashboard. Counters are per-rebuild-pass.
  clio::run::u64 rows_to_rebuild = 0;
  for (size_t gi = 0; gi < groups_.size(); ++gi) {
    const Group &g = *groups_[gi];
    if (is_data && idx >= g.k_) continue;
    rows_to_rebuild += g.num_rows_;
  }
  recovery_ops_total_.store(rows_to_rebuild, std::memory_order_relaxed);
  recovery_ops_completed_.store(0, std::memory_order_relaxed);
  recovery_ops_in_flight_.store(0, std::memory_order_relaxed);
  recovering_is_parity_.store(is_data ? 0u : 1u, std::memory_order_relaxed);
  recovering_index_.store(idx, std::memory_order_relaxed);
  recovery_active_.store(1, std::memory_order_release);

  clio::run::u64 rows_done = 0;
  for (size_t gi = 0; gi < groups_.size(); ++gi) {
    const Group &g = *groups_[gi];
    if (is_data && idx >= g.k_) continue;  // column absent in this narrower group
    for (clio::run::u64 r = g.first_row_; r < g.LastRow(); ++r) {
      if (max_rows != 0 && rows_done >= max_rows) {
        completed = false;  // interrupted: leave the member recovering
        recovery_active_.store(0, std::memory_order_release);
        CLIO_CO_RETURN;
      }
      if (is_data && IsRowDirty(r)) {
        HLOG(kError,
             "safe_bdev RebuildMember: row {} dirty (parity not built); cannot "
             "reconstruct data member",
             r);
        ok = false;
        recovery_active_.store(0, std::memory_order_release);
        CLIO_CO_RETURN;
      }
      std::vector<std::vector<uint8_t>> data_chunks;
      bool rd_ok = false;
      const int exclude_col = is_data ? idx : -1;
      CLIO_CO_AWAIT(ReconstructRow(g, r, exclude_col, data_chunks, rd_ok));
      if (!rd_ok) {
        ok = false;
        recovery_active_.store(0, std::memory_order_release);
        CLIO_CO_RETURN;
      }
      ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kChunkLen);
      if (buf.IsNull()) {
        ok = false;
        recovery_active_.store(0, std::memory_order_release);
        CLIO_CO_RETURN;
      }
      if (is_data) {
        std::memcpy(buf.ptr_, data_chunks[static_cast<size_t>(idx)].data(),
                    kChunkLen);
      } else {
        std::vector<const uint8_t *> ptrs(static_cast<size_t>(g.k_));
        for (int c = 0; c < g.k_; ++c) {
          ptrs[static_cast<size_t>(c)] =
              data_chunks[static_cast<size_t>(c)].data();
        }
        g.rs_->EncodeParityShard(idx, ptrs, kChunkLen,
                                 reinterpret_cast<uint8_t *>(buf.ptr_));
      }
      recovery_ops_in_flight_.store(1, std::memory_order_relaxed);
      auto fut = client.AsyncWrite(MemberQuery(),
                                   MemberBlocks(ChunkOffset(r), kChunkLen),
                                   buf.shm_.template Cast<void>(), kChunkLen);
      CLIO_CO_AWAIT(fut);
      const bool wok =
          (fut->return_code_ == 0) && (fut->bytes_written_ == kChunkLen);
      ipc->FreeBuffer(buf);
      recovery_ops_in_flight_.store(0, std::memory_order_relaxed);
      if (!wok) {
        ok = false;
        recovery_active_.store(0, std::memory_order_release);
        CLIO_CO_RETURN;
      }
      recovery_ops_completed_.fetch_add(1, std::memory_order_relaxed);
      ++rows_done;
    }
  }
  recovery_active_.store(0, std::memory_order_release);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::RecoverBdev(clio::run::shared_ptr<RecoverBdevTask> &task) {
  CLIO_TASK_BODY_BEGIN

  // Locate the failed member by its (old) pool id, in data then parity.
  int failed_col = -1;
  int failed_par = -1;
  for (size_t i = 0; i < data_members_.size(); ++i) {
    if (data_members_[i].pool_id_ == task->old_bdev_id_) {
      failed_col = static_cast<int>(i);
      break;
    }
  }
  if (failed_col < 0) {
    for (size_t i = 0; i < parity_members_.size(); ++i) {
      if (parity_members_[i].pool_id_ == task->old_bdev_id_) {
        failed_par = static_cast<int>(i);
        break;
      }
    }
  }
  if (failed_col < 0 && failed_par < 0) {
    HLOG(kError, "safe_bdev RecoverBdev: old member {} not found",
         task->old_bdev_id_.ToU64());
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }

  const bool is_data = (failed_col >= 0);
  const int idx = is_data ? failed_col : failed_par;

  // Seat the fresh member on its new pool and mark it RECOVERING: it stays
  // non-active (excluded from reconstruction; degraded reads serve data) until
  // the rebuild completes. Record the recovery INTENT in the manifest BEFORE
  // rebuilding so a crash mid-rebuild is resumed by the next Create().
  auto &client = is_data ? data_clients_[static_cast<size_t>(idx)]
                         : parity_clients_[static_cast<size_t>(idx)];
  client = clio::run::bdev::Client(task->new_pool_id_);
  MemberSlot &m = is_data ? data_members_[static_cast<size_t>(idx)]
                          : parity_members_[static_cast<size_t>(idx)];
  m.pool_id_ = task->new_pool_id_;
  m.pool_name_ = task->pool_name_.str();
  m.node_id_ = task->node_id_;
  m.state_ = ec::EcState::kFaulty;  // non-active until rebuild completes
  m.recovering_ = true;
  PersistMemberManifest();

  HLOG(kInfo, "safe_bdev RecoverBdev: rebuilding {} member {} onto '{}'",
       is_data ? "data" : "parity", idx, task->pool_name_.str());

  bool ok = false;
  bool completed = false;
  CLIO_CO_AWAIT(RebuildMember(is_data, idx, ok, completed));
  if (!ok) {
    // Rebuild failed (I/O / too few survivors). Leave recovering set so a retry
    // or the next Create() can resume.
    task->return_code_ = 1;
    CLIO_CO_RETURN;
  }
  if (!completed) {
    HLOG(kWarning,
         "safe_bdev RecoverBdev: rebuild interrupted (test hook) -- recovery "
         "persisted; the next Create() resumes it");
    task->return_code_ = 0;  // partial but consistent (member still recovering)
    CLIO_CO_RETURN;
  }

  // Rebuild finished: bring the member online and stamp its superblock.
  m.state_ = ec::EcState::kActive;
  m.recovering_ = false;
  {
    bool wr_ok = false;
    CLIO_CO_AWAIT(WriteSuperblock(!is_data, static_cast<size_t>(idx), wr_ok));
    if (!wr_ok) {
      HLOG(kWarning,
           "safe_bdev RecoverBdev: superblock write failed for recovered "
           "member (data reconstructed; will be re-stamped on next attach)");
    }
  }
  PersistMemberManifest();
  HLOG(kInfo, "safe_bdev RecoverBdev: {} member {} recovered onto '{}'",
       is_data ? "data" : "parity", idx, task->pool_name_.str());
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::ResumeRecoveries() {
  CLIO_TASK_BODY_BEGIN
  // Any member left recovering (crash mid-RecoverBdev) is rebuilt from scratch
  // now -- RebuildMember is idempotent, so re-running is safe and the member
  // stays excluded from reads until it completes.
  for (size_t i = 0; i < data_members_.size(); ++i) {
    if (!data_members_[i].recovering_) continue;
    HLOG(kInfo,
         "safe_bdev: resuming interrupted recovery of data member {} onto '{}'",
         i, data_members_[i].pool_name_);
    bool ok = false;
    bool completed = false;
    CLIO_CO_AWAIT(RebuildMember(true, static_cast<int>(i), ok, completed));
    if (ok && completed) {
      data_members_[i].state_ = ec::EcState::kActive;
      data_members_[i].recovering_ = false;
      bool wr = false;
      CLIO_CO_AWAIT(WriteSuperblock(false, i, wr));
      PersistMemberManifest();
      HLOG(kInfo, "safe_bdev: recovery of data member {} completed on restart",
           i);
    } else {
      HLOG(kWarning,
           "safe_bdev: resume of data member {} did not complete "
           "(ok={}, completed={})",
           i, ok, completed);
    }
  }
  for (size_t j = 0; j < parity_members_.size(); ++j) {
    if (!parity_members_[j].recovering_) continue;
    bool ok = false;
    bool completed = false;
    CLIO_CO_AWAIT(RebuildMember(false, static_cast<int>(j), ok, completed));
    if (ok && completed) {
      parity_members_[j].state_ = ec::EcState::kActive;
      parity_members_[j].recovering_ = false;
      bool wr = false;
      CLIO_CO_AWAIT(WriteSuperblock(true, j, wr));
      PersistMemberManifest();
    }
  }
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::BuildParity(clio::run::shared_ptr<BuildParityTask> &task) {
  CLIO_TASK_BODY_BEGIN
  if (parity_level_ == 0) {
    // No parity configured: nothing to protect. Drop pending marks.
    std::lock_guard<std::mutex> g(row_mu_);
    dirty_rows_.clear();
    task->return_code_ = 0;
    CLIO_CO_RETURN;
  }

  // Snapshot a batch of dirty GLOBAL rows (max_batch_==0 drains all). Rows are
  // erased only AFTER their parity is durably written, so an observer that sees
  // an empty dirty set knows every row is protected (single periodic builder,
  // rescheduled only after completion). A row that cannot be built (a data
  // member is down) is left dirty for a later pass.
  std::vector<clio::run::u64> batch;
  {
    std::lock_guard<std::mutex> g(row_mu_);
    for (clio::run::u64 r : dirty_rows_) {
      batch.push_back(r);
      if (task->max_batch_ != 0 &&
          batch.size() >= static_cast<size_t>(task->max_batch_)) {
        break;
      }
    }
  }

  auto *ipc = CLIO_IPC;
  clio::run::u32 built = 0;
  for (clio::run::u64 r : batch) {
    // Find the group owning this global row; parity is computed under its code.
    const int gi = FindGroupByRow(r);
    if (gi < 0) {
      // Stale dirty mark for a row no longer owned by any group; drop it.
      std::lock_guard<std::mutex> g(row_mu_);
      dirty_rows_.erase(r);
      continue;
    }
    const Group &g = *groups_[static_cast<size_t>(gi)];
    const int kg = g.k_;

    // Read the k_g FULL data chunks at this row offset (all of the group's data
    // members must be active to build parity over the full chunks).
    std::vector<std::vector<uint8_t>> data_chunks(
        static_cast<size_t>(kg), std::vector<uint8_t>(kChunkLen, 0));
    bool rd_ok = true;
    for (int c = 0; c < kg; ++c) {
      if (data_members_[static_cast<size_t>(c)].state_ !=
          ec::EcState::kActive) {
        rd_ok = false;
        break;
      }
      bool one = false;
      CLIO_CO_AWAIT(ReadDataSegment(static_cast<size_t>(c), ChunkOffset(r),
                                    data_chunks[static_cast<size_t>(c)].data(),
                                    kChunkLen, one));
      rd_ok = one;
      if (!rd_ok) {
        break;
      }
    }
    if (!rd_ok) {
      continue;  // leave dirty; retry next pass
    }

    std::vector<const uint8_t *> ptrs(static_cast<size_t>(kg));
    for (int c = 0; c < kg; ++c) {
      ptrs[static_cast<size_t>(c)] = data_chunks[static_cast<size_t>(c)].data();
    }

    // (Re)compute and write each active parity member's chunk for this row,
    // under this group's code (rs_g over k_g data chunks).
    bool wr_ok = true;
    for (int j = 0; j < static_cast<int>(parity_level_); ++j) {
      if (parity_members_[static_cast<size_t>(j)].state_ !=
          ec::EcState::kActive) {
        continue;
      }
      ctp::ipc::FullPtr<char> buf = ipc->AllocateBuffer(kChunkLen);
      if (buf.IsNull()) {
        wr_ok = false;
        break;
      }
      g.rs_->EncodeParityShard(j, ptrs, kChunkLen,
                               reinterpret_cast<uint8_t *>(buf.ptr_));
      auto fut = parity_clients_[static_cast<size_t>(j)].AsyncWrite(
          MemberQuery(), MemberBlocks(ChunkOffset(r), kChunkLen),
          buf.shm_.template Cast<void>(), kChunkLen);
      CLIO_CO_AWAIT(fut);
      wr_ok = (fut->return_code_ == 0) && (fut->bytes_written_ == kChunkLen);
      MaybeFaultParity(static_cast<size_t>(j), fut->io_error_);
      ipc->FreeBuffer(buf);
      if (!wr_ok) {
        break;
      }
    }
    if (!wr_ok) {
      continue;  // leave dirty; retry next pass
    }
    {
      std::lock_guard<std::mutex> g2(row_mu_);
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

clio::run::TaskResume Runtime::FlushAllocLog(
    clio::run::shared_ptr<FlushAllocLogTask> &task) {
  CLIO_TASK_BODY_BEGIN
  // Append buffered records to disk (also folds them into the in-memory
  // recovered model). Idempotent when the buffer is empty.
  alloc_log_.Flush();
  // Compact when the on-disk record count has grown past the threshold:
  // max(kMinCompactRecords, live * kCompactGrowthFactor). Compaction rewrites
  // the log down to one group-open per group + one record per live block,
  // bounding the file size. Mirrors bdev's FlushAllocLog.
  const clio::run::u64 live = alloc_log_.live_block_count();
  const clio::run::u64 on_disk = alloc_log_.records_on_disk();
  const clio::run::u64 threshold =
      std::max<clio::run::u64>(kMinCompactRecords, live * kCompactGrowthFactor);
  if (on_disk > threshold) {
    alloc_log_.Compact();
  }
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Monitor(clio::run::shared_ptr<MonitorTask> &task) {
  CLIO_TASK_BODY_BEGIN
  if (task->query_ == "stats") {
    clio::run::u32 dirty = 0;
    {
      std::lock_guard<std::mutex> g(row_mu_);
      dirty = static_cast<clio::run::u32>(dirty_rows_.size());
    }
    // Snapshot the recovery counters (see safe_bdev_runtime.h). remaining is
    // derived so the dashboard never has to reconcile total/completed itself.
    const clio::run::u64 rec_total =
        recovery_ops_total_.load(std::memory_order_relaxed);
    const clio::run::u64 rec_done =
        recovery_ops_completed_.load(std::memory_order_relaxed);
    const clio::run::u64 rec_inflight =
        recovery_ops_in_flight_.load(std::memory_order_relaxed);
    const clio::run::u64 rec_remaining =
        (rec_total > rec_done) ? (rec_total - rec_done) : 0;
    const clio::run::u32 rec_active =
        recovery_active_.load(std::memory_order_acquire);
    const clio::run::u32 rec_is_par =
        recovering_is_parity_.load(std::memory_order_relaxed);
    const int rec_idx = recovering_index_.load(std::memory_order_relaxed);

    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(sbuf);
    pk.pack_map(15);
    pk.pack("pool_name");     pk.pack(pool_name_);
    pk.pack("max_failures");  pk.pack(max_failures_);
    pk.pack("data_count");
    pk.pack(static_cast<clio::run::u32>(data_members_.size()));
    pk.pack("parity_level");  pk.pack(parity_level_);
    pk.pack("num_groups");
    pk.pack(static_cast<clio::run::u32>(groups_.size()));
    pk.pack("total_rows");    pk.pack(total_rows_);
    pk.pack("dirty_rows");    pk.pack(dirty);
    pk.pack("reattached_members"); pk.pack(reattached_members_);
    pk.pack("alloc_log_records");
    pk.pack(static_cast<clio::run::u64>(alloc_log_.records_on_disk()));
    // Recovery observability: live rebuild progress for the dashboard.
    pk.pack("recovery_active");        pk.pack(rec_active);
    pk.pack("recovery_ops_total");     pk.pack(rec_total);
    pk.pack("recovery_ops_completed"); pk.pack(rec_done);
    pk.pack("recovery_ops_in_flight"); pk.pack(rec_inflight);
    pk.pack("recovery_ops_remaining"); pk.pack(rec_remaining);
    // Per-member roster: role / index / name / pool_id / state (+ recovering
    // flag). Powers the member list and the add / remove / recover controls in
    // the context-visualizer safe-bdev dashboard.
    const auto pack_members = [&](const std::vector<MemberSlot> &vec,
                                  bool is_parity) {
      for (const auto &m : vec) {
        const char *st = (m.state_ == ec::EcState::kFaulty)  ? "faulty"
                         : (m.state_ == ec::EcState::kRemoved) ? "removed"
                                                               : "active";
        const bool recovering = (rec_active != 0) &&
                                ((is_parity ? 1u : 0u) == rec_is_par) &&
                                (m.index_ == rec_idx);
        pk.pack_map(6);
        pk.pack("role");
        pk.pack(std::string(is_parity ? "parity" : "data"));
        pk.pack("index");     pk.pack(static_cast<clio::run::u32>(m.index_));
        pk.pack("pool_name"); pk.pack(m.pool_name_);
        pk.pack("pool_id");   pk.pack(m.pool_id_.ToU64());
        pk.pack("state");
        pk.pack(std::string(recovering ? "recovering" : st));
        pk.pack("recovering");
        pk.pack(static_cast<clio::run::u32>(recovering ? 1 : 0));
      }
    };
    pk.pack("members");
    pk.pack_array(static_cast<uint32_t>(data_members_.size() +
                                        parity_members_.size()));
    pack_members(data_members_, false);
    pack_members(parity_members_, true);
    task->results_[container_id_] = std::string(sbuf.data(), sbuf.size());
  }
  task->SetReturnCode(0);
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::TaskResume Runtime::Destroy(clio::run::shared_ptr<DestroyTask> &task) {
  CLIO_TASK_BODY_BEGIN
  // Persist any buffered allocator-state records before teardown so a clean
  // pool destroy leaves a recoverable log on disk.
  alloc_log_.Flush();
  // Member clients/state are released by their destructors.
  task->return_code_ = 0;
  CLIO_CO_RETURN;
  CLIO_TASK_BODY_END
}

clio::run::u64 Runtime::GetWorkRemaining() const { return 0; }

}  // namespace clio::run::safe_bdev

// Define ChiMod entry points using CLIO_TASK_CC macro
CLIO_TASK_CC(clio::run::safe_bdev::Runtime)

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
 * Daemon-level end-to-end test for the safe_bdev ChiMod (issue #543).
 *
 * Exercises the RAID-0-data + dedicated-parity model over RAM-backed member
 * bdevs:
 *   1. Roundtrip + recovery: k=3 data + 1 parity (max_failures=1); write a
 *      multi-chunk striped pattern; read it back; flush parity; fault a data
 *      member; degraded read reconstructs; RecoverBdev onto a fresh member;
 *      read back.
 *   2. RAID-0 striping: a > k*kChunkLen write physically lands on DIFFERENT
 *      data members (verified by reading members directly at mapped offsets).
 *   3. Reclaim: allocate, free, allocate again -> the space is reused
 *      (allocator is not a bump pointer).
 *   4. Superblock reattach + foreign-refuse.
 *   5. Partial-chunk write/read.
 */

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "simple_test.h"

using namespace std::chrono_literals;

#include <clio_runtime/clio_runtime.h>
#include <clio_runtime/pool_query.h>
#include <clio_runtime/singletons.h>
#include <clio_runtime/types.h>

// Reused bdev client (members are plain bdev pools).
#include <clio_runtime/bdev/bdev_client.h>
#include <clio_runtime/bdev/bdev_tasks.h>

// safe_bdev client and tasks.
#include <clio_runtime/safe_bdev/safe_bdev_client.h>
#include <clio_runtime/safe_bdev/safe_bdev_tasks.h>

// Admin client for pool management.
#include <clio_runtime/admin/admin_client.h>
#include <clio_runtime/admin/admin_tasks.h>

// msgpack for parsing Monitor "stats" output (reattached_members).
#include <clio_ctp/serialize/msgpack_wrapper.h>

namespace {

bool g_initialized = false;

// Per-member RAM size: must hold the reserved EC region. The runtime reserves
// num_rows * kChunkLen per member where kChunkLen = 64KiB. 4 MiB comfortably
// holds many rows.
constexpr clio::run::u64 kMemberRamSize = 4 * 1024 * 1024;
constexpr clio::run::u64 kChunkLen = 65536;        // mirrors Runtime::kChunkLen
constexpr clio::run::u64 kSuperblockSize = 65536;  // mirrors Runtime::kSuperblockSize

/** Initialize Chimaera once for the test suite. */
void EnsureInit() {
  if (!g_initialized) {
    HLOG(kInfo, "Initializing Chimaera (safe_bdev test)...");
    bool success = clio::run::CHIMAERA_INIT(clio::run::ChimaeraMode::kClient, true);
    if (success) {
      g_initialized = true;
      SimpleTest::g_test_finalize = clio::run::CHIMAERA_FINALIZE;
      std::this_thread::sleep_for(500ms);
      HLOG(kInfo, "Chimaera initialization successful");
    } else {
      HLOG(kInfo, "Failed to initialize Chimaera");
    }
  }
}

/** Create a RAM-backed member bdev pool. */
bool CreateRamMember(clio::run::bdev::Client &client,
                     const std::string &pool_name,
                     const clio::run::PoolId &pool_id) {
  auto create_task = client.AsyncCreate(clio::run::PoolQuery::Dynamic(), pool_name,
                                        pool_id, clio::run::bdev::BdevType::kRam,
                                        kMemberRamSize);
  create_task.Wait();
  client.pool_id_ = create_task->new_pool_id_;
  client.return_code_ = create_task->return_code_;
  return create_task->GetReturnCode() == 0;
}

/**
 * Create a FILE-backed member bdev pool over `file_path`. File-backed members
 * persist their bytes across a safe-bdev pool destroy, which is what makes the
 * restart/recovery test meaningful: re-creating a bdev pool over the SAME file
 * re-exposes the SAME data (including the safe-bdev superblock).
 */
bool CreateFileMember(clio::run::bdev::Client &client,
                      const std::string &file_path,
                      const clio::run::PoolId &pool_id) {
  auto create_task = client.AsyncCreate(
      clio::run::PoolQuery::Dynamic(), file_path, pool_id,
      clio::run::bdev::BdevType::kFile, kMemberRamSize);
  create_task.Wait();
  client.pool_id_ = create_task->new_pool_id_;
  client.return_code_ = create_task->return_code_;
  return create_task->GetReturnCode() == 0;
}

/** Whether two [offset,size) logical ranges overlap. */
bool RangesOverlap(clio::run::u64 a_off, clio::run::u64 a_size, clio::run::u64 b_off,
                   clio::run::u64 b_size) {
  return a_off < b_off + b_size && b_off < a_off + a_size;
}

/** A repeating, position-dependent byte pattern. */
std::vector<ctp::u8> MakePattern(size_t size, ctp::u8 seed) {
  std::vector<ctp::u8> v(size);
  for (size_t i = 0; i < size; ++i) {
    v[i] = static_cast<ctp::u8>((seed + i * 7 + (i >> 8)) & 0xFF);
  }
  return v;
}

}  // namespace

TEST_CASE("safe_bdev_ec_roundtrip_recovery", "[safe_bdev][ec][recovery]") {
  EnsureInit();
  REQUIRE(g_initialized);

  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "safe_member_" + std::to_string(getpid()) + "_" +
           std::to_string(idx);
  };

  // --- Create k=3 RAM data member pools + 1 parity. ---
  const int k = 3;
  std::vector<clio::run::PoolId> data_ids;
  std::vector<clio::run::bdev::Client> data_clients;
  for (int c = 0; c < k; ++c) {
    clio::run::PoolId id(static_cast<clio::run::u32>(9100 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
    data_clients.push_back(client);
  }

  // Parity member.
  clio::run::PoolId parity_id(static_cast<clio::run::u32>(9200 + pidsalt), 0);
  clio::run::bdev::Client parity_client(parity_id);
  REQUIRE(CreateRamMember(parity_client, member_name(100), parity_id));
  parity_id = parity_client.pool_id_;

  // --- Create safe_bdev over the 3 data members, max_failures=1. ---
  clio::run::PoolId safe_id(static_cast<clio::run::u32>(9300 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);

  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  auto create_task = safe.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                      "safe_bdev_ec_pool", safe_id,
                                      /*max_failures=*/1, members);
  create_task.Wait();
  safe.pool_id_ = create_task->new_pool_id_;
  REQUIRE(create_task->GetReturnCode() == 0);

  // --- AddBdev the parity member so parity_level becomes 1. ---
  auto add_task = safe.AsyncAddBdev(clio::run::PoolQuery::Dynamic(),
                                    member_name(100), /*node_id=*/0, parity_id,
                                    /*as_parity=*/1);
  add_task.Wait();
  REQUIRE(add_task->GetReturnCode() == 0);

  // --- Allocate a logical block spanning MULTIPLE chunks so it stripes across
  //     all data members, write a known pattern, read it back. ---
  const clio::run::u64 io_len = 2 * static_cast<clio::run::u64>(k) * kChunkLen;  // 6 chunks
  auto alloc = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), io_len);
  alloc.Wait();
  REQUIRE(alloc->GetReturnCode() == 0);
  REQUIRE(alloc->blocks_.size() > 0);
  clio::run::bdev::Block block = alloc->blocks_[0];

  std::vector<ctp::u8> pattern = MakePattern(io_len, 0x5A);

  clio::run::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
  wblocks.push_back(block);

  auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
  REQUIRE_FALSE(wbuf.IsNull());
  memcpy(wbuf.ptr_, pattern.data(), io_len);
  auto write_task = safe.AsyncWrite(
      clio::run::PoolQuery::Dynamic(), wblocks,
      wbuf.shm_.template Cast<void>(), io_len);
  write_task.Wait();
  REQUIRE(write_task->GetReturnCode() == 0);
  REQUIRE(write_task->bytes_written_ == io_len);

  auto read_back = [&](std::vector<ctp::u8> &out) {
    clio::run::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
    rblocks.push_back(block);
    auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, io_len);
    auto read_task = safe.AsyncRead(
        clio::run::PoolQuery::Dynamic(), rblocks,
        rbuf.shm_.template Cast<void>(), io_len);
    read_task.Wait();
    REQUIRE(read_task->GetReturnCode() == 0);
    REQUIRE(read_task->bytes_read_ == io_len);
    out.resize(io_len);
    memcpy(out.data(), rbuf.ptr_, io_len);
    CLIO_IPC->FreeBuffer(rbuf);
  };

  // (1) Happy-path roundtrip.
  {
    std::vector<ctp::u8> got;
    read_back(got);
    REQUIRE(got == pattern);
    HLOG(kInfo, "safe_bdev EC: happy-path roundtrip OK ({} bytes)", io_len);
  }

  // (2) Degraded read: fault data member 1, reconstruct on read.
  {
    // Parity is computed asynchronously off the write path (the background
    // BuildParity task). Flush it as a durability barrier so the rows are
    // protected before we induce a failure.
    auto flush = safe.AsyncBuildParity(clio::run::PoolQuery::Dynamic(),
                                       /*max_batch=*/0);
    flush.Wait();
    REQUIRE(flush->GetReturnCode() == 0);

    auto rm = safe.AsyncRemoveBdev(clio::run::PoolQuery::Dynamic(), data_ids[1],
                                   /*was_faulty=*/1);
    rm.Wait();
    REQUIRE(rm->GetReturnCode() == 0);

    std::vector<ctp::u8> got;
    read_back(got);
    REQUIRE(got == pattern);
    HLOG(kInfo, "safe_bdev EC: degraded read (member 1 faulty) reconstructed OK");
  }

  // (3) Recovery onto a fresh member pool.
  {
    clio::run::PoolId recover_id(static_cast<clio::run::u32>(9400 + pidsalt), 0);
    clio::run::bdev::Client recover_client(recover_id);
    REQUIRE(CreateRamMember(recover_client, member_name(200), recover_id));
    recover_id = recover_client.pool_id_;

    auto rec = safe.AsyncRecoverBdev(clio::run::PoolQuery::Dynamic(), data_ids[1],
                                     member_name(200), /*node_id=*/0,
                                     recover_id);
    rec.Wait();
    REQUIRE(rec->GetReturnCode() == 0);

    std::vector<ctp::u8> got;
    read_back(got);
    REQUIRE(got == pattern);
    HLOG(kInfo, "safe_bdev EC: post-recovery read OK");

    // Confirm redundancy restored: fault a DIFFERENT member, still readable.
    auto rm2 = safe.AsyncRemoveBdev(clio::run::PoolQuery::Dynamic(), data_ids[0],
                                    /*was_faulty=*/1);
    rm2.Wait();
    REQUIRE(rm2->GetReturnCode() == 0);

    std::vector<ctp::u8> got2;
    read_back(got2);
    REQUIRE(got2 == pattern);
    HLOG(kInfo, "safe_bdev EC: degraded read after recovery (member 0 faulty) OK");
  }

  CLIO_IPC->FreeBuffer(wbuf);
  HLOG(kInfo, "safe_bdev EC end-to-end test PASSED");
}

TEST_CASE("safe_bdev_raid0_striping", "[safe_bdev][ec][striping]") {
  EnsureInit();
  REQUIRE(g_initialized);
  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "st_member_" + std::to_string(getpid()) + "_" + std::to_string(idx);
  };

  // --- k=3 data members, no parity needed for this test. ---
  const int k = 3;
  std::vector<clio::run::PoolId> data_ids;
  std::vector<clio::run::bdev::Client> data_clients;
  for (int c = 0; c < k; ++c) {
    clio::run::PoolId id(static_cast<clio::run::u32>(9800 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
    data_clients.push_back(client);
  }

  clio::run::PoolId safe_id(static_cast<clio::run::u32>(9850 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  auto create_task = safe.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                      "safe_bdev_st_pool", safe_id,
                                      /*max_failures=*/1, members);
  create_task.Wait();
  safe.pool_id_ = create_task->new_pool_id_;
  REQUIRE(create_task->GetReturnCode() == 0);

  // Allocate/write a buffer > k*kChunkLen so it lands on different members.
  const clio::run::u64 io_len = 2 * static_cast<clio::run::u64>(k) * kChunkLen;  // 6 chunks
  auto alloc = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), io_len);
  alloc.Wait();
  REQUIRE(alloc->GetReturnCode() == 0);
  clio::run::bdev::Block block = alloc->blocks_[0];
  // Expect the allocator to hand out logical offset 0 for the first alloc.
  REQUIRE(block.offset_ == 0);

  std::vector<ctp::u8> pattern = MakePattern(io_len, 0x3C);
  clio::run::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
  wblocks.push_back(block);
  auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
  REQUIRE_FALSE(wbuf.IsNull());
  memcpy(wbuf.ptr_, pattern.data(), io_len);
  auto wt = safe.AsyncWrite(clio::run::PoolQuery::Dynamic(), wblocks,
                            wbuf.shm_.template Cast<void>(), io_len);
  wt.Wait();
  REQUIRE(wt->GetReturnCode() == 0);
  CLIO_IPC->FreeBuffer(wbuf);

  // Verify physically: chunk ci lands on data member (ci % k) at member offset
  // kSuperblockSize + (ci/k)*kChunkLen. Read each data member directly via its
  // bdev client and confirm the chunk equals the corresponding slice of the
  // pattern. Because offset 0 -> chunk 0 -> member 0, chunk 1 -> member 1, etc,
  // distinct chunks land on DIFFERENT members.
  const clio::run::u64 num_chunks = io_len / kChunkLen;  // 6
  for (clio::run::u64 ci = 0; ci < num_chunks; ++ci) {
    const int data_col = static_cast<int>(ci % static_cast<clio::run::u64>(k));
    const clio::run::u64 row = ci / static_cast<clio::run::u64>(k);
    const clio::run::u64 phys = kSuperblockSize + row * kChunkLen;

    clio::run::priv::vector<clio::run::bdev::Block> rb(CTP_MALLOC);
    rb.push_back(clio::run::bdev::Block(phys, kChunkLen, 0));
    auto rbuf = CLIO_IPC->AllocateBuffer(kChunkLen);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, kChunkLen);
    auto rt = data_clients[data_col].AsyncRead(
        clio::run::PoolQuery::Local(), rb, rbuf.shm_.template Cast<void>(),
        kChunkLen);
    rt.Wait();
    REQUIRE(rt->GetReturnCode() == 0);
    std::vector<ctp::u8> got(kChunkLen);
    memcpy(got.data(), rbuf.ptr_, kChunkLen);
    std::vector<ctp::u8> expect(pattern.begin() + ci * kChunkLen,
                                pattern.begin() + (ci + 1) * kChunkLen);
    REQUIRE(got == expect);
    CLIO_IPC->FreeBuffer(rbuf);
  }
  HLOG(kInfo,
       "safe_bdev RAID-0: {} chunks verified striped across {} data members",
       num_chunks, k);
}

TEST_CASE("safe_bdev_reclaim", "[safe_bdev][ec][reclaim]") {
  EnsureInit();
  REQUIRE(g_initialized);
  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "rc_member_" + std::to_string(getpid()) + "_" + std::to_string(idx);
  };

  const int k = 3;
  std::vector<clio::run::PoolId> data_ids;
  for (int c = 0; c < k; ++c) {
    clio::run::PoolId id(static_cast<clio::run::u32>(9900 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }
  clio::run::PoolId safe_id(static_cast<clio::run::u32>(9950 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  auto create_task = safe.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                      "safe_bdev_rc_pool", safe_id,
                                      /*max_failures=*/1, members);
  create_task.Wait();
  safe.pool_id_ = create_task->new_pool_id_;
  REQUIRE(create_task->GetReturnCode() == 0);

  const clio::run::u64 sz = static_cast<clio::run::u64>(k) * kChunkLen;

  // Allocate, capture the offset, free, allocate again: the SAME region should
  // be reused (the allocator is a free-list, not a bump pointer).
  auto a1 = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), sz);
  a1.Wait();
  REQUIRE(a1->GetReturnCode() == 0);
  REQUIRE(a1->blocks_.size() > 0);
  const clio::run::u64 off1 = a1->blocks_[0].offset_;

  clio::run::priv::vector<clio::run::bdev::Block> fblocks(CTP_MALLOC);
  fblocks.push_back(a1->blocks_[0]);
  auto fr = safe.AsyncFreeBlocks(clio::run::PoolQuery::Dynamic(), fblocks);
  fr.Wait();
  REQUIRE(fr->GetReturnCode() == 0);

  auto a2 = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), sz);
  a2.Wait();
  REQUIRE(a2->GetReturnCode() == 0);
  REQUIRE(a2->blocks_.size() > 0);
  const clio::run::u64 off2 = a2->blocks_[0].offset_;

  REQUIRE(off1 == off2);
  HLOG(kInfo,
       "safe_bdev reclaim: freed region at offset {} reused by next alloc "
       "(offset {})",
       off1, off2);
}

TEST_CASE("safe_bdev_partial_chunk", "[safe_bdev][ec][partial]") {
  EnsureInit();
  REQUIRE(g_initialized);
  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "pc_member_" + std::to_string(getpid()) + "_" + std::to_string(idx);
  };

  const int k = 3;
  std::vector<clio::run::PoolId> data_ids;
  for (int c = 0; c < k; ++c) {
    clio::run::PoolId id(static_cast<clio::run::u32>(10000 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }
  clio::run::PoolId parity_id(static_cast<clio::run::u32>(10060 + pidsalt), 0);
  clio::run::bdev::Client parity_client(parity_id);
  REQUIRE(CreateRamMember(parity_client, member_name(50), parity_id));
  parity_id = parity_client.pool_id_;

  clio::run::PoolId safe_id(static_cast<clio::run::u32>(10080 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  auto create_task = safe.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                      "safe_bdev_pc_pool", safe_id,
                                      /*max_failures=*/1, members);
  create_task.Wait();
  safe.pool_id_ = create_task->new_pool_id_;
  REQUIRE(create_task->GetReturnCode() == 0);

  auto add = safe.AsyncAddBdev(clio::run::PoolQuery::Dynamic(), member_name(50),
                               /*node_id=*/0, parity_id, /*as_parity=*/1);
  add.Wait();
  REQUIRE(add->GetReturnCode() == 0);

  // A partial-chunk write: 100000 bytes spans chunk 0 (full 64KiB) + part of
  // chunk 1, i.e. an arbitrary, non-chunk-aligned length.
  const clio::run::u64 io_len = 100000;
  auto alloc = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), io_len);
  alloc.Wait();
  REQUIRE(alloc->GetReturnCode() == 0);
  clio::run::bdev::Block block = alloc->blocks_[0];

  std::vector<ctp::u8> pattern = MakePattern(io_len, 0x9E);
  clio::run::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
  wblocks.push_back(clio::run::bdev::Block(block.offset_, io_len, 0));
  auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
  REQUIRE_FALSE(wbuf.IsNull());
  memcpy(wbuf.ptr_, pattern.data(), io_len);
  auto wt = safe.AsyncWrite(clio::run::PoolQuery::Dynamic(), wblocks,
                            wbuf.shm_.template Cast<void>(), io_len);
  wt.Wait();
  REQUIRE(wt->GetReturnCode() == 0);
  REQUIRE(wt->bytes_written_ == io_len);
  CLIO_IPC->FreeBuffer(wbuf);

  // Flush parity, fault a data member, degraded read of the partial range.
  auto flush = safe.AsyncBuildParity(clio::run::PoolQuery::Dynamic(), 0);
  flush.Wait();
  REQUIRE(flush->GetReturnCode() == 0);

  auto rm = safe.AsyncRemoveBdev(clio::run::PoolQuery::Dynamic(), data_ids[1],
                                 /*was_faulty=*/1);
  rm.Wait();
  REQUIRE(rm->GetReturnCode() == 0);

  clio::run::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
  rblocks.push_back(clio::run::bdev::Block(block.offset_, io_len, 0));
  auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
  REQUIRE_FALSE(rbuf.IsNull());
  memset(rbuf.ptr_, 0, io_len);
  auto rt = safe.AsyncRead(clio::run::PoolQuery::Dynamic(), rblocks,
                           rbuf.shm_.template Cast<void>(), io_len);
  rt.Wait();
  REQUIRE(rt->GetReturnCode() == 0);
  REQUIRE(rt->bytes_read_ == io_len);
  std::vector<ctp::u8> got(io_len);
  memcpy(got.data(), rbuf.ptr_, io_len);
  REQUIRE(got == pattern);
  CLIO_IPC->FreeBuffer(rbuf);
  HLOG(kInfo, "safe_bdev partial-chunk write/degraded-read OK ({} bytes)",
       io_len);
}

namespace {

/**
 * Query safe_bdev Monitor("stats") and extract an unsigned-integer field by
 * key. Returns -1 if the field/blob could not be parsed.
 */
long QueryStatField(clio::run::safe_bdev::Client &safe, const char *field) {
  auto mon = safe.AsyncMonitor(clio::run::PoolQuery::Dynamic(), "stats");
  mon.Wait();
  if (mon->GetReturnCode() != 0) {
    return -1;
  }
  for (const auto &kv_blob : mon->results_) {
    const std::string &blob = kv_blob.second;
    if (blob.empty()) {
      continue;
    }
    msgpack::object_handle oh = msgpack::unpack(blob.data(), blob.size());
    const msgpack::object &obj = oh.get();
    if (obj.type != msgpack::type::MAP) {
      continue;
    }
    for (uint32_t j = 0; j < obj.via.map.size; ++j) {
      const auto &kv = obj.via.map.ptr[j];
      std::string key;
      kv.key.convert(key);
      if (key == field) {
        clio::run::u64 v = 0;
        kv.val.convert(v);
        return static_cast<long>(v);
      }
    }
  }
  return -1;
}

/** Back-compat shim for the reattach test. */
long QueryReattachedMembers(clio::run::safe_bdev::Client &safe) {
  return QueryStatField(safe, "reattached_members");
}

}  // namespace

TEST_CASE("safe_bdev_superblock_reattach", "[safe_bdev][superblock][reattach]") {
  EnsureInit();
  REQUIRE(g_initialized);
  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "sb_member_" + std::to_string(getpid()) + "_" + std::to_string(idx);
  };

  // --- Create k=3 fresh RAM data members. ---
  const int k = 3;
  std::vector<clio::run::PoolId> data_ids;
  for (int c = 0; c < k; ++c) {
    clio::run::PoolId id(static_cast<clio::run::u32>(9600 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }

  // --- Phase 1: create safe_bdev pool X over the fresh members. ---
  clio::run::PoolId safe_id(static_cast<clio::run::u32>(9650 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  {
    auto create_task = safe.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                        "safe_bdev_sb_poolX", safe_id,
                                        /*max_failures=*/1, members);
    create_task.Wait();
    safe.pool_id_ = create_task->new_pool_id_;
    REQUIRE(create_task->GetReturnCode() == 0);
  }

  // Fresh members must NOT be reattached.
  REQUIRE(QueryReattachedMembers(safe) == 0);

  // Write + flush + read a multi-chunk block so the members hold real data.
  const clio::run::u64 io_len = static_cast<clio::run::u64>(k) * kChunkLen;
  auto alloc = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), io_len);
  alloc.Wait();
  REQUIRE(alloc->GetReturnCode() == 0);
  REQUIRE(alloc->blocks_.size() > 0);
  clio::run::bdev::Block block = alloc->blocks_[0];
  std::vector<ctp::u8> pattern = MakePattern(io_len, 0x77);

  {
    clio::run::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
    wblocks.push_back(block);
    auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(wbuf.IsNull());
    memcpy(wbuf.ptr_, pattern.data(), io_len);
    auto wt = safe.AsyncWrite(clio::run::PoolQuery::Dynamic(), wblocks,
                              wbuf.shm_.template Cast<void>(), io_len);
    wt.Wait();
    REQUIRE(wt->GetReturnCode() == 0);
    REQUIRE(wt->bytes_written_ == io_len);
    CLIO_IPC->FreeBuffer(wbuf);
  }
  {
    auto flush = safe.AsyncBuildParity(clio::run::PoolQuery::Dynamic(), 0);
    flush.Wait();
    REQUIRE(flush->GetReturnCode() == 0);
  }
  {
    clio::run::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
    rblocks.push_back(block);
    auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, io_len);
    auto rt = safe.AsyncRead(clio::run::PoolQuery::Dynamic(), rblocks,
                             rbuf.shm_.template Cast<void>(), io_len);
    rt.Wait();
    REQUIRE(rt->GetReturnCode() == 0);
    std::vector<ctp::u8> got(io_len);
    memcpy(got.data(), rbuf.ptr_, io_len);
    REQUIRE(got == pattern);
    CLIO_IPC->FreeBuffer(rbuf);
  }
  HLOG(kInfo, "safe_bdev SB: phase 1 (fresh create + roundtrip) OK");

  // --- Tear down array X (members + their superblocks persist as independent
  //     bdev pools). Destroying first forces a genuine re-attach that re-reads
  //     each member's superblock. ---
  {
    clio::run::admin::Client admin(clio::run::kAdminPoolId);
    auto destroy = admin.AsyncDestroyPool(clio::run::PoolQuery::Dynamic(),
                                          safe.pool_id_);
    destroy.Wait();
    REQUIRE(destroy->GetReturnCode() == 0);
    std::this_thread::sleep_for(100ms);
  }

  // --- Phase 2: create AGAIN with the SAME pool id X over the SAME members.
  //     Every member already carries our superblock => all re-attached. ---
  clio::run::PoolId safe_id2 = safe_id;  // same identity
  clio::run::safe_bdev::Client safe2(safe_id2);
  {
    auto create_task = safe2.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                         "safe_bdev_sb_poolX", safe_id2,
                                         /*max_failures=*/1, members);
    create_task.Wait();
    safe2.pool_id_ = create_task->new_pool_id_;
    REQUIRE(create_task->GetReturnCode() == 0);
  }
  long reattached = QueryReattachedMembers(safe2);
  HLOG(kInfo, "safe_bdev SB: re-attach reported reattached_members={}",
       reattached);
  REQUIRE(reattached > 0);
  REQUIRE(reattached == k);
  HLOG(kInfo, "safe_bdev superblock reattach test PASSED");
}

TEST_CASE("safe_bdev_superblock_foreign_refuse",
          "[safe_bdev][superblock][foreign]") {
  EnsureInit();
  REQUIRE(g_initialized);
  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "fr_member_" + std::to_string(getpid()) + "_" + std::to_string(idx);
  };

  // --- Create k=3 fresh RAM data members. ---
  const int k = 3;
  std::vector<clio::run::PoolId> data_ids;
  for (int c = 0; c < k; ++c) {
    clio::run::PoolId id(static_cast<clio::run::u32>(9700 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }

  // --- Array X claims the members (writes superblocks). ---
  clio::run::PoolId safe_idX(static_cast<clio::run::u32>(9750 + pidsalt), 0);
  clio::run::safe_bdev::Client safeX(safe_idX);
  {
    auto create_task = safeX.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                         "safe_bdev_fr_poolX", safe_idX,
                                         /*max_failures=*/1, members);
    create_task.Wait();
    safeX.pool_id_ = create_task->new_pool_id_;
    REQUIRE(create_task->GetReturnCode() == 0);
  }

  // --- Array Y (DIFFERENT pool id) over the SAME members => must REFUSE. ---
  clio::run::PoolId safe_idY(static_cast<clio::run::u32>(9760 + pidsalt), 0);
  clio::run::safe_bdev::Client safeY(safe_idY);
  {
    auto create_task = safeY.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                         "safe_bdev_fr_poolY", safe_idY,
                                         /*max_failures=*/1, members);
    create_task.Wait();
    HLOG(kInfo, "safe_bdev FR: foreign create returned rc={}",
         create_task->GetReturnCode());
    REQUIRE(create_task->GetReturnCode() != 0);
  }
  HLOG(kInfo, "safe_bdev superblock foreign-refuse test PASSED");
}

TEST_CASE("safe_bdev_add_data_no_reshuffle",
          "[safe_bdev][ec][add_data][groups]") {
  EnsureInit();
  REQUIRE(g_initialized);
  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "ad_member_" + std::to_string(getpid()) + "_" + std::to_string(idx);
  };

  // --- Group 0: k=2 data + 1 parity (max_failures=1). ---
  const int k0 = 2;
  std::vector<clio::run::PoolId> data_ids;
  for (int c = 0; c < k0; ++c) {
    clio::run::PoolId id(static_cast<clio::run::u32>(10100 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }
  clio::run::PoolId parity_id(static_cast<clio::run::u32>(10160 + pidsalt), 0);
  clio::run::bdev::Client parity_client(parity_id);
  REQUIRE(CreateRamMember(parity_client, member_name(50), parity_id));
  parity_id = parity_client.pool_id_;

  clio::run::PoolId safe_id(static_cast<clio::run::u32>(10180 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k0; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  auto create_task = safe.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                      "safe_bdev_ad_pool", safe_id,
                                      /*max_failures=*/1, members);
  create_task.Wait();
  safe.pool_id_ = create_task->new_pool_id_;
  REQUIRE(create_task->GetReturnCode() == 0);

  auto add_par = safe.AsyncAddBdev(clio::run::PoolQuery::Dynamic(), member_name(50),
                                   /*node_id=*/0, parity_id, /*as_parity=*/1);
  add_par.Wait();
  REQUIRE(add_par->GetReturnCode() == 0);
  REQUIRE(QueryStatField(safe, "num_groups") == 1);

  // Generic alloc+write+flush+read+verify helper. Returns the block so the
  // caller can re-read it later (to assert it stays unchanged).
  auto write_pattern = [&](clio::run::u64 io_len, ctp::u8 seed,
                           clio::run::bdev::Block &out_block,
                           std::vector<ctp::u8> &out_pattern) {
    auto alloc = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), io_len);
    alloc.Wait();
    REQUIRE(alloc->GetReturnCode() == 0);
    REQUIRE(alloc->blocks_.size() > 0);
    out_block = alloc->blocks_[0];
    out_pattern = MakePattern(io_len, seed);

    clio::run::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
    wblocks.push_back(clio::run::bdev::Block(out_block.offset_, io_len, 0));
    auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(wbuf.IsNull());
    memcpy(wbuf.ptr_, out_pattern.data(), io_len);
    auto wt = safe.AsyncWrite(clio::run::PoolQuery::Dynamic(), wblocks,
                              wbuf.shm_.template Cast<void>(), io_len);
    wt.Wait();
    REQUIRE(wt->GetReturnCode() == 0);
    REQUIRE(wt->bytes_written_ == io_len);
    CLIO_IPC->FreeBuffer(wbuf);

    auto flush = safe.AsyncBuildParity(clio::run::PoolQuery::Dynamic(), 0);
    flush.Wait();
    REQUIRE(flush->GetReturnCode() == 0);
  };

  auto read_verify = [&](const clio::run::bdev::Block &block, clio::run::u64 io_len,
                         const std::vector<ctp::u8> &expect) {
    clio::run::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
    rblocks.push_back(clio::run::bdev::Block(block.offset_, io_len, 0));
    auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, io_len);
    auto rt = safe.AsyncRead(clio::run::PoolQuery::Dynamic(), rblocks,
                             rbuf.shm_.template Cast<void>(), io_len);
    rt.Wait();
    REQUIRE(rt->GetReturnCode() == 0);
    REQUIRE(rt->bytes_read_ == io_len);
    std::vector<ctp::u8> got(io_len);
    memcpy(got.data(), rbuf.ptr_, io_len);
    REQUIRE(got == expect);
    CLIO_IPC->FreeBuffer(rbuf);
  };

  // --- Pattern A in group 0 (k=2): 4 chunks. ---
  const clio::run::u64 lenA = 2 * static_cast<clio::run::u64>(k0) * kChunkLen;  // 4 chunks
  clio::run::bdev::Block blockA;
  std::vector<ctp::u8> patternA;
  write_pattern(lenA, 0xA1, blockA, patternA);
  read_verify(blockA, lenA, patternA);
  HLOG(kInfo, "safe_bdev add-data: pattern A written+verified in group 0 "
              "(offset {}, {} bytes)", blockA.offset_, lenA);

  // --- AddBdev a 3rd DATA member: must succeed (no longer an error) and open
  //     group 1 (k=3). ---
  clio::run::PoolId data3_id(static_cast<clio::run::u32>(10170 + pidsalt), 0);
  clio::run::bdev::Client data3_client(data3_id);
  REQUIRE(CreateRamMember(data3_client, member_name(2), data3_id));
  data3_id = data3_client.pool_id_;
  data_ids.push_back(data3_id);

  auto add_data = safe.AsyncAddBdev(clio::run::PoolQuery::Dynamic(), member_name(2),
                                    /*node_id=*/0, data3_id, /*as_parity=*/0);
  add_data.Wait();
  REQUIRE(add_data->GetReturnCode() == 0);  // <-- the fix: no longer an error
  REQUIRE(QueryStatField(safe, "num_groups") == 2);
  REQUIRE(QueryStatField(safe, "data_count") == 3);
  HLOG(kInfo, "safe_bdev add-data: 3rd data member added, num_groups=2");

  // --- Pattern B in group 1 (k=3): 6 chunks. ---
  const clio::run::u64 lenB = 2 * 3 * kChunkLen;  // 6 chunks across 3 data drives
  clio::run::bdev::Block blockB;
  std::vector<ctp::u8> patternB;
  write_pattern(lenB, 0xB2, blockB, patternB);
  read_verify(blockB, lenB, patternB);
  // Group 1's logical offsets must lie ABOVE group 0's range.
  REQUIRE(blockB.offset_ >= lenA);
  HLOG(kInfo, "safe_bdev add-data: pattern B written+verified in group 1 "
              "(offset {}, {} bytes)", blockB.offset_, lenB);

  // --- NO RESHUFFLE: re-read pattern A; it must be UNCHANGED. ---
  read_verify(blockA, lenA, patternA);
  HLOG(kInfo, "safe_bdev add-data: pattern A UNCHANGED after add (no reshuffle)");

  // --- Fault data member 0 (shared by BOTH groups) and confirm degraded reads
  //     reconstruct in BOTH groups, each under its own k. ---
  auto rm = safe.AsyncRemoveBdev(clio::run::PoolQuery::Dynamic(), data_ids[0],
                                 /*was_faulty=*/1);
  rm.Wait();
  REQUIRE(rm->GetReturnCode() == 0);

  read_verify(blockA, lenA, patternA);  // group 0 (k=2) degraded
  HLOG(kInfo, "safe_bdev add-data: degraded read of pattern A (group 0, k=2) OK");
  read_verify(blockB, lenB, patternB);  // group 1 (k=3) degraded
  HLOG(kInfo, "safe_bdev add-data: degraded read of pattern B (group 1, k=3) OK");

  HLOG(kInfo, "safe_bdev add-data no-reshuffle test PASSED");
}

TEST_CASE("safe_bdev_alloc_log_restart_recovery",
          "[safe_bdev][alloc_log][recover][groups]") {
  EnsureInit();
  REQUIRE(g_initialized);
  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_file = [&](int idx) {
    return std::string("/tmp/safe_alog_member_") + std::to_string(getpid()) +
           "_" + std::to_string(idx) + ".bin";
  };
  const std::string log_path =
      std::string("/tmp/safe_alog_") + std::to_string(getpid()) + ".alog";

  // Fresh start: remove any stale member files + alloc log.
  for (int i = 0; i < 4; ++i) {
    std::filesystem::remove(member_file(i));
  }
  std::filesystem::remove(log_path);

  // The safe-bdev pool id is STABLE across the restart so the recovered array
  // recognizes its own superblocks (reattach, not foreign).
  const clio::run::PoolId safe_id(static_cast<clio::run::u32>(10500 + pidsalt), 0);

  // k=2 data + 1 parity. Group 0 has k=2; after AddBdev a 3rd data member,
  // group 1 has k=3.
  const int k0 = 2;

  // Patterns: A lands in group 0 (k=2, 4 chunks); B lands in group 1 (k=3,
  // 6 chunks). Captured here so phase 2 can verify them post-recovery.
  const clio::run::u64 lenA = 2 * static_cast<clio::run::u64>(k0) * kChunkLen;  // 4 chunks
  const clio::run::u64 lenB = 2 * 3 * kChunkLen;                          // 6 chunks
  std::vector<ctp::u8> patternA = MakePattern(lenA, 0xA1);
  std::vector<ctp::u8> patternB = MakePattern(lenB, 0xB2);
  clio::run::bdev::Block blockA;
  clio::run::bdev::Block blockB;

  // Member pool ids (reused verbatim in phase 2 so each bdev pool re-opens the
  // SAME backing file).
  std::vector<clio::run::PoolId> data_ids;
  clio::run::PoolId parity_id(static_cast<clio::run::u32>(10560 + pidsalt), 0);
  clio::run::PoolId data3_id(static_cast<clio::run::u32>(10570 + pidsalt), 0);

  // A write+flush(parity) helper bound to a given safe client.
  auto write_pattern = [&](clio::run::safe_bdev::Client &safe, clio::run::u64 io_len,
                           const std::vector<ctp::u8> &pattern,
                           clio::run::bdev::Block &out_block) {
    auto alloc = safe.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), io_len);
    alloc.Wait();
    REQUIRE(alloc->GetReturnCode() == 0);
    REQUIRE(alloc->blocks_.size() > 0);
    out_block = alloc->blocks_[0];

    clio::run::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
    wblocks.push_back(clio::run::bdev::Block(out_block.offset_, io_len, 0));
    auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(wbuf.IsNull());
    memcpy(wbuf.ptr_, pattern.data(), io_len);
    auto wt = safe.AsyncWrite(clio::run::PoolQuery::Dynamic(), wblocks,
                              wbuf.shm_.template Cast<void>(), io_len);
    wt.Wait();
    REQUIRE(wt->GetReturnCode() == 0);
    REQUIRE(wt->bytes_written_ == io_len);
    CLIO_IPC->FreeBuffer(wbuf);

    auto flush = safe.AsyncBuildParity(clio::run::PoolQuery::Dynamic(), 0);
    flush.Wait();
    REQUIRE(flush->GetReturnCode() == 0);
  };

  auto read_verify = [&](clio::run::safe_bdev::Client &safe,
                         const clio::run::bdev::Block &block, clio::run::u64 io_len,
                         const std::vector<ctp::u8> &expect) {
    clio::run::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
    rblocks.push_back(clio::run::bdev::Block(block.offset_, io_len, 0));
    auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, io_len);
    auto rt = safe.AsyncRead(clio::run::PoolQuery::Dynamic(), rblocks,
                             rbuf.shm_.template Cast<void>(), io_len);
    rt.Wait();
    REQUIRE(rt->GetReturnCode() == 0);
    REQUIRE(rt->bytes_read_ == io_len);
    std::vector<ctp::u8> got(io_len);
    memcpy(got.data(), rbuf.ptr_, io_len);
    REQUIRE(got == expect);
    CLIO_IPC->FreeBuffer(rbuf);
  };

  // ======================================================================
  // PHASE 1: create, write A (group 0), add a 3rd data member (open group 1),
  //          write B (group 1), flush the alloc log, then destroy the pool.
  // ======================================================================
  {
    std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
    for (int c = 0; c < k0; ++c) {
      clio::run::PoolId id(static_cast<clio::run::u32>(10510 + pidsalt + c), 0);
      clio::run::bdev::Client client(id);
      REQUIRE(CreateFileMember(client, member_file(c), id));
      data_ids.push_back(client.pool_id_);
      members.emplace_back(member_file(c), /*node_id=*/0, client.pool_id_);
    }
    clio::run::bdev::Client parity_client(parity_id);
    REQUIRE(CreateFileMember(parity_client, member_file(3), parity_id));
    parity_id = parity_client.pool_id_;

    clio::run::safe_bdev::Client safe(safe_id);
    auto create_task = safe.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                        "safe_bdev_alog_pool", safe_id,
                                        /*max_failures=*/1, members, log_path);
    create_task.Wait();
    safe.pool_id_ = create_task->new_pool_id_;
    REQUIRE(create_task->GetReturnCode() == 0);

    auto add_par = safe.AsyncAddBdev(clio::run::PoolQuery::Dynamic(), member_file(3),
                                     /*node_id=*/0, parity_id, /*as_parity=*/1);
    add_par.Wait();
    REQUIRE(add_par->GetReturnCode() == 0);
    REQUIRE(QueryStatField(safe, "num_groups") == 1);

    // Pattern A in group 0 (k=2).
    write_pattern(safe, lenA, patternA, blockA);
    read_verify(safe, blockA, lenA, patternA);
    HLOG(kInfo, "safe_bdev alog: phase1 pattern A in group 0 (offset {})",
         blockA.offset_);

    // Add a 3rd DATA member => opens group 1 (k=3).
    clio::run::bdev::Client data3_client(data3_id);
    REQUIRE(CreateFileMember(data3_client, member_file(2), data3_id));
    data3_id = data3_client.pool_id_;
    data_ids.push_back(data3_id);
    auto add_data = safe.AsyncAddBdev(clio::run::PoolQuery::Dynamic(), member_file(2),
                                      /*node_id=*/0, data3_id, /*as_parity=*/0);
    add_data.Wait();
    REQUIRE(add_data->GetReturnCode() == 0);
    REQUIRE(QueryStatField(safe, "num_groups") == 2);
    REQUIRE(QueryStatField(safe, "data_count") == 3);

    // Pattern B in group 1 (k=3); its logical offset is above group 0's range.
    write_pattern(safe, lenB, patternB, blockB);
    read_verify(safe, blockB, lenB, patternB);
    REQUIRE(blockB.offset_ >= lenA);
    HLOG(kInfo, "safe_bdev alog: phase1 pattern B in group 1 (offset {})",
         blockB.offset_);

    // Explicit one-shot alloc-log flush barrier (mirrors bdev's WAL test): the
    // PoolManager DestroyPool does not invoke the container Destroy handler, so
    // flush the buffered records before tearing the pool down.
    {
      auto fl = safe.AsyncFlushAllocLog(clio::run::PoolQuery::Dynamic(), /*period=*/0);
      fl.Wait();
      REQUIRE(fl->GetReturnCode() == 0);
    }
    {
      auto fl2 = safe.AsyncBuildParity(clio::run::PoolQuery::Dynamic(), 0);
      fl2.Wait();
      REQUIRE(fl2->GetReturnCode() == 0);
    }

    clio::run::admin::Client admin(clio::run::kAdminPoolId);
    auto destroy = admin.AsyncDestroyPool(clio::run::PoolQuery::Dynamic(),
                                          safe.pool_id_);
    destroy.Wait();
    REQUIRE(destroy->GetReturnCode() == 0);
    std::this_thread::sleep_for(150ms);
  }

  // ======================================================================
  // PHASE 2 (RESTART): create a NEW safe-bdev pool over the SAME member files
  //          (re-opened bdev pools) + SAME alloc_log_path + SAME safe pool id.
  //          Assert recovery: num_groups==2, A and B intact, and a fresh alloc
  //          does NOT collide with the recovered live blocks.
  // ======================================================================
  {
    // Re-open the bdev member pools over the SAME backing files (fresh bdev
    // pool ids; the files — and thus the bytes + superblocks — persist).
    std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
    for (int c = 0; c < 3; ++c) {
      clio::run::PoolId id(static_cast<clio::run::u32>(10580 + pidsalt + c), 0);
      clio::run::bdev::Client client(id);
      REQUIRE(CreateFileMember(client, member_file(c), id));
      members.emplace_back(member_file(c), /*node_id=*/0, client.pool_id_);
    }
    clio::run::PoolId rparity_id(static_cast<clio::run::u32>(10590 + pidsalt), 0);
    clio::run::bdev::Client rparity_client(rparity_id);
    REQUIRE(CreateFileMember(rparity_client, member_file(3), rparity_id));
    rparity_id = rparity_client.pool_id_;

    clio::run::safe_bdev::Client safe2(safe_id);
    auto create_task = safe2.AsyncCreate(clio::run::PoolQuery::Dynamic(),
                                         "safe_bdev_alog_pool", safe_id,
                                         /*max_failures=*/1, members, log_path);
    create_task.Wait();
    safe2.pool_id_ = create_task->new_pool_id_;
    REQUIRE(create_task->GetReturnCode() == 0);

    // (a) The append-only group structure recovered: 2 groups, 3 data members.
    const long ng = QueryStatField(safe2, "num_groups");
    HLOG(kInfo, "safe_bdev alog: RESTART recovered num_groups={}", ng);
    REQUIRE(ng == 2);
    REQUIRE(QueryStatField(safe2, "data_count") == 3);
    // The members carry our superblock => they re-attach (not fresh/foreign).
    REQUIRE(QueryReattachedMembers(safe2) == 3);

    // Re-attach parity so degraded paths remain available (data still intact;
    // this also re-dirties rows but we only read active members here).
    auto add_par = safe2.AsyncAddBdev(clio::run::PoolQuery::Dynamic(), member_file(3),
                                      /*node_id=*/0, rparity_id,
                                      /*as_parity=*/1);
    add_par.Wait();
    REQUIRE(add_par->GetReturnCode() == 0);

    // (b) Pattern A (group 0) AND pattern B (group 1) read back correctly —
    //     data intact on the file members + correct group/allocator recovery.
    read_verify(safe2, blockA, lenA, patternA);
    HLOG(kInfo, "safe_bdev alog: RESTART pattern A intact (group 0, offset {})",
         blockA.offset_);
    read_verify(safe2, blockB, lenB, patternB);
    HLOG(kInfo, "safe_bdev alog: RESTART pattern B intact (group 1, offset {})",
         blockB.offset_);

    // (c) A fresh allocation must NOT overlap the recovered live blocks of A/B.
    auto alloc = safe2.AsyncAllocateBlocks(clio::run::PoolQuery::Dynamic(), kChunkLen);
    alloc.Wait();
    REQUIRE(alloc->GetReturnCode() == 0);
    REQUIRE(alloc->blocks_.size() > 0);
    const clio::run::bdev::Block nb = alloc->blocks_[0];
    REQUIRE_FALSE(RangesOverlap(nb.offset_, nb.size_, blockA.offset_, lenA));
    REQUIRE_FALSE(RangesOverlap(nb.offset_, nb.size_, blockB.offset_, lenB));
    HLOG(kInfo,
         "safe_bdev alog: RESTART fresh alloc at offset {} (size {}) does NOT "
         "collide with recovered A/B",
         nb.offset_, nb.size_);

    clio::run::admin::Client admin(clio::run::kAdminPoolId);
    auto destroy = admin.AsyncDestroyPool(clio::run::PoolQuery::Dynamic(),
                                          safe2.pool_id_);
    destroy.Wait();
  }

  // Cleanup backing files + log.
  for (int i = 0; i < 4; ++i) {
    std::filesystem::remove(member_file(i));
  }
  std::filesystem::remove(log_path);
  HLOG(kInfo, "safe_bdev alloc-log restart/recovery test PASSED");
}

SIMPLE_TEST_MAIN()

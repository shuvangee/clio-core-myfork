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
 * Daemon-level end-to-end test for the safe_bdev ChiMod (issue #543, Part B).
 *
 * Builds RAM-backed member bdev pools and a safe_bdev erasure-coded device over
 * them, then exercises the full async data plane:
 *   1. Happy path: write a known pattern across data members + inline parity,
 *      read it back, assert equality.
 *   2. Degraded read: fault one data member, read the same block, assert the
 *      reconstructed data still equals the original.
 *   3. Recovery: RecoverBdev onto a fresh member pool, read back, assert
 *      equality (and fault a different member to confirm redundancy restored).
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
// num_stripes * kShardLen per member where kShardLen = 64KiB. 4 MiB comfortably
// holds several stripes.
constexpr chi::u64 kMemberRamSize = 4 * 1024 * 1024;
constexpr chi::u64 kShardLen = 65536;  // mirrors Runtime::kShardLen

/** Initialize Chimaera once for the test suite. */
void EnsureInit() {
  if (!g_initialized) {
    HLOG(kInfo, "Initializing Chimaera (safe_bdev test)...");
    bool success = chi::CHIMAERA_INIT(chi::ChimaeraMode::kClient, true);
    if (success) {
      g_initialized = true;
      SimpleTest::g_test_finalize = chi::CHIMAERA_FINALIZE;
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
                     const chi::PoolId &pool_id) {
  auto create_task = client.AsyncCreate(chi::PoolQuery::Dynamic(), pool_name,
                                        pool_id, clio::run::bdev::BdevType::kRam,
                                        kMemberRamSize);
  create_task.Wait();
  client.pool_id_ = create_task->new_pool_id_;
  client.return_code_ = create_task->return_code_;
  return create_task->GetReturnCode() == 0;
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

  // --- Create k=3 RAM data member pools + 1 parity + 1 spare-for-recovery. ---
  const int k = 3;
  std::vector<chi::PoolId> data_ids;
  std::vector<clio::run::bdev::Client> data_clients;
  for (int c = 0; c < k; ++c) {
    chi::PoolId id(static_cast<chi::u32>(9100 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
    data_clients.push_back(client);
  }

  // Parity member.
  chi::PoolId parity_id(static_cast<chi::u32>(9200 + pidsalt), 0);
  clio::run::bdev::Client parity_client(parity_id);
  REQUIRE(CreateRamMember(parity_client, member_name(100), parity_id));
  parity_id = parity_client.pool_id_;

  // --- Create safe_bdev over the 3 data members, max_failures=1. ---
  chi::PoolId safe_id(static_cast<chi::u32>(9300 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);

  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  auto create_task = safe.AsyncCreate(chi::PoolQuery::Dynamic(),
                                      "safe_bdev_ec_pool", safe_id,
                                      /*max_failures=*/1, members);
  create_task.Wait();
  safe.pool_id_ = create_task->new_pool_id_;
  REQUIRE(create_task->GetReturnCode() == 0);

  // --- AddBdev the parity member so parity_level becomes 1. ---
  auto add_task = safe.AsyncAddBdev(chi::PoolQuery::Dynamic(),
                                    member_name(100), /*node_id=*/0, parity_id,
                                    /*as_parity=*/1);
  add_task.Wait();
  REQUIRE(add_task->GetReturnCode() == 0);

  // --- Allocate a logical block (one full stripe), write, read back. ---
  const chi::u64 io_len = static_cast<chi::u64>(k) * kShardLen;  // one stripe
  auto alloc = safe.AsyncAllocateBlocks(chi::PoolQuery::Dynamic(), io_len);
  alloc.Wait();
  REQUIRE(alloc->GetReturnCode() == 0);
  REQUIRE(alloc->blocks_.size() > 0);
  clio::run::bdev::Block block = alloc->blocks_[0];

  std::vector<ctp::u8> pattern = MakePattern(io_len, 0x5A);

  chi::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
  wblocks.push_back(block);

  auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
  REQUIRE_FALSE(wbuf.IsNull());
  memcpy(wbuf.ptr_, pattern.data(), io_len);
  auto write_task = safe.AsyncWrite(
      chi::PoolQuery::Dynamic(), wblocks,
      wbuf.shm_.template Cast<void>(), io_len);
  write_task.Wait();
  REQUIRE(write_task->GetReturnCode() == 0);
  REQUIRE(write_task->bytes_written_ == io_len);

  auto read_back = [&](std::vector<ctp::u8> &out) {
    chi::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
    rblocks.push_back(block);
    auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, io_len);
    auto read_task = safe.AsyncRead(
        chi::PoolQuery::Dynamic(), rblocks,
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
    // BuildParity task). Flush it as a durability barrier so the stripe is
    // protected before we induce a failure — otherwise the stripe would be in
    // its bounded unprotected window and reconstruction would (correctly)
    // refuse.
    auto flush = safe.AsyncBuildParity(chi::PoolQuery::Dynamic(),
                                       /*max_batch=*/0);
    flush.Wait();
    REQUIRE(flush->GetReturnCode() == 0);

    auto rm = safe.AsyncRemoveBdev(chi::PoolQuery::Dynamic(), data_ids[1],
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
    chi::PoolId recover_id(static_cast<chi::u32>(9400 + pidsalt), 0);
    clio::run::bdev::Client recover_client(recover_id);
    REQUIRE(CreateRamMember(recover_client, member_name(200), recover_id));
    recover_id = recover_client.pool_id_;

    auto rec = safe.AsyncRecoverBdev(chi::PoolQuery::Dynamic(), data_ids[1],
                                     member_name(200), /*node_id=*/0,
                                     recover_id);
    rec.Wait();
    REQUIRE(rec->GetReturnCode() == 0);

    std::vector<ctp::u8> got;
    read_back(got);
    REQUIRE(got == pattern);
    HLOG(kInfo, "safe_bdev EC: post-recovery read OK");

    // Confirm redundancy restored: fault a DIFFERENT member, still readable.
    auto rm2 = safe.AsyncRemoveBdev(chi::PoolQuery::Dynamic(), data_ids[0],
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

TEST_CASE("safe_bdev_variable_width_generations",
          "[safe_bdev][ec][generations]") {
  EnsureInit();
  REQUIRE(g_initialized);

  std::this_thread::sleep_for(100ms);

  const int pidsalt = static_cast<int>(getpid() & 0xFFF);
  auto member_name = [&](int idx) {
    return "vw_member_" + std::to_string(getpid()) + "_" + std::to_string(idx);
  };

  // --- gen0: k=3 RAM data members + 1 parity. ---
  const int k0 = 3;
  std::vector<chi::PoolId> data_ids;
  for (int c = 0; c < k0; ++c) {
    chi::PoolId id(static_cast<chi::u32>(9500 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }
  chi::PoolId parity_id(static_cast<chi::u32>(9560 + pidsalt), 0);
  clio::run::bdev::Client parity_client(parity_id);
  REQUIRE(CreateRamMember(parity_client, member_name(50), parity_id));
  parity_id = parity_client.pool_id_;

  // 4th DATA member that opens gen1 (k=4).
  chi::PoolId data4_id(static_cast<chi::u32>(9570 + pidsalt), 0);
  clio::run::bdev::Client data4_client(data4_id);
  REQUIRE(CreateRamMember(data4_client, member_name(60), data4_id));
  data4_id = data4_client.pool_id_;

  // --- Create safe_bdev over the 3 data members, max_failures=1. ---
  chi::PoolId safe_id(static_cast<chi::u32>(9580 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k0; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  auto create_task = safe.AsyncCreate(chi::PoolQuery::Dynamic(),
                                      "safe_bdev_vw_pool", safe_id,
                                      /*max_failures=*/1, members);
  create_task.Wait();
  safe.pool_id_ = create_task->new_pool_id_;
  REQUIRE(create_task->GetReturnCode() == 0);

  // Add the parity member (parity_level -> 1 on generation 0).
  auto add_par = safe.AsyncAddBdev(chi::PoolQuery::Dynamic(), member_name(50),
                                   /*node_id=*/0, parity_id, /*as_parity=*/1);
  add_par.Wait();
  REQUIRE(add_par->GetReturnCode() == 0);

  // Helpers to allocate + write + read a stripe of a given width.
  auto write_stripe = [&](chi::u64 io_len, ctp::u8 seed,
                          clio::run::bdev::Block *out_block,
                          std::vector<ctp::u8> *out_pattern) {
    auto alloc = safe.AsyncAllocateBlocks(chi::PoolQuery::Dynamic(), io_len);
    alloc.Wait();
    REQUIRE(alloc->GetReturnCode() == 0);
    REQUIRE(alloc->blocks_.size() > 0);
    *out_block = alloc->blocks_[0];

    *out_pattern = MakePattern(io_len, seed);
    chi::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
    wblocks.push_back(*out_block);
    auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(wbuf.IsNull());
    memcpy(wbuf.ptr_, out_pattern->data(), io_len);
    auto wt = safe.AsyncWrite(chi::PoolQuery::Dynamic(), wblocks,
                              wbuf.shm_.template Cast<void>(), io_len);
    wt.Wait();
    REQUIRE(wt->GetReturnCode() == 0);
    REQUIRE(wt->bytes_written_ == io_len);
    CLIO_IPC->FreeBuffer(wbuf);
  };
  auto read_stripe = [&](const clio::run::bdev::Block &block, chi::u64 io_len,
                         std::vector<ctp::u8> *out) {
    chi::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
    rblocks.push_back(block);
    auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, io_len);
    auto rt = safe.AsyncRead(chi::PoolQuery::Dynamic(), rblocks,
                             rbuf.shm_.template Cast<void>(), io_len);
    rt.Wait();
    REQUIRE(rt->GetReturnCode() == 0);
    REQUIRE(rt->bytes_read_ == io_len);
    out->resize(io_len);
    memcpy(out->data(), rbuf.ptr_, io_len);
    CLIO_IPC->FreeBuffer(rbuf);
  };
  auto flush_parity = [&]() {
    auto flush = safe.AsyncBuildParity(chi::PoolQuery::Dynamic(),
                                       /*max_batch=*/0);
    flush.Wait();
    REQUIRE(flush->GetReturnCode() == 0);
  };

  // Several stripes per generation so the per-stripe parity rotation places
  // every member (in particular the soon-to-fault member 0) as DATA in some
  // stripes and PARITY in others — exercising genuine reconstruction.
  const int kStripesPerGen = 4;

  // --- Phase 1: write + flush + read kStripesPerGen gen0 stripes (k=3). ---
  const chi::u64 len0 = static_cast<chi::u64>(k0) * kShardLen;
  std::vector<clio::run::bdev::Block> blocks0;
  std::vector<std::vector<ctp::u8>> patterns0;
  for (int i = 0; i < kStripesPerGen; ++i) {
    clio::run::bdev::Block b;
    std::vector<ctp::u8> p;
    write_stripe(len0, static_cast<ctp::u8>(0x11 + i), &b, &p);
    blocks0.push_back(b);
    patterns0.push_back(std::move(p));
  }
  flush_parity();
  for (int i = 0; i < kStripesPerGen; ++i) {
    std::vector<ctp::u8> got;
    read_stripe(blocks0[i], len0, &got);
    REQUIRE(got == patterns0[i]);
  }
  HLOG(kInfo, "safe_bdev VW: gen0 (k=3) roundtrip OK ({} stripes)",
       kStripesPerGen);

  // --- Phase 2: AddBdev a 4th DATA member -> opens gen1 (k=4). ---
  auto add_data = safe.AsyncAddBdev(chi::PoolQuery::Dynamic(), member_name(60),
                                    /*node_id=*/0, data4_id, /*as_parity=*/0);
  add_data.Wait();
  REQUIRE(add_data->GetReturnCode() == 0);
  HLOG(kInfo, "safe_bdev VW: added 4th data member (opened gen1, k=4)");

  // --- Phase 3: write + flush + read kStripesPerGen gen1 stripes (k=4). ---
  const int k1 = 4;
  const chi::u64 len1 = static_cast<chi::u64>(k1) * kShardLen;
  std::vector<clio::run::bdev::Block> blocks1;
  std::vector<std::vector<ctp::u8>> patterns1;
  for (int i = 0; i < kStripesPerGen; ++i) {
    clio::run::bdev::Block b;
    std::vector<ctp::u8> p;
    write_stripe(len1, static_cast<ctp::u8>(0xC0 + i), &b, &p);
    blocks1.push_back(b);
    patterns1.push_back(std::move(p));
  }
  flush_parity();
  for (int i = 0; i < kStripesPerGen; ++i) {
    std::vector<ctp::u8> got;
    read_stripe(blocks1[i], len1, &got);
    REQUIRE(got == patterns1[i]);
  }
  HLOG(kInfo, "safe_bdev VW: gen1 (k=4) roundtrip OK ({} stripes)",
       kStripesPerGen);

  // --- Phase 4: read BOTH generations back (non-degraded). ---
  for (int i = 0; i < kStripesPerGen; ++i) {
    std::vector<ctp::u8> g0;
    std::vector<ctp::u8> g1;
    read_stripe(blocks0[i], len0, &g0);
    read_stripe(blocks1[i], len1, &g1);
    REQUIRE(g0 == patterns0[i]);
    REQUIRE(g1 == patterns1[i]);
  }
  HLOG(kInfo, "safe_bdev VW: both generations readable after widening");

  // --- Phase 5: fault a data member participating in BOTH generations
  //     (member 0) and confirm degraded reads reconstruct in each. Across
  //     kStripesPerGen stripes member 0 holds DATA in some, forcing decode. ---
  {
    flush_parity();  // ensure both generations' parity is current
    auto rm = safe.AsyncRemoveBdev(chi::PoolQuery::Dynamic(), data_ids[0],
                                   /*was_faulty=*/1);
    rm.Wait();
    REQUIRE(rm->GetReturnCode() == 0);

    for (int i = 0; i < kStripesPerGen; ++i) {
      std::vector<ctp::u8> g0;
      std::vector<ctp::u8> g1;
      read_stripe(blocks0[i], len0, &g0);
      read_stripe(blocks1[i], len1, &g1);
      REQUIRE(g0 == patterns0[i]);
      REQUIRE(g1 == patterns1[i]);
    }
    HLOG(kInfo,
         "safe_bdev VW: degraded reads reconstruct in BOTH generations after "
         "faulting a shared data member");
  }

  HLOG(kInfo, "safe_bdev variable-width generations test PASSED");
}

namespace {

/**
 * Query safe_bdev Monitor("stats") and extract the "reattached_members" field.
 * Returns -1 if the field/blob could not be parsed.
 */
long QueryReattachedMembers(clio::run::safe_bdev::Client &safe) {
  auto mon = safe.AsyncMonitor(chi::PoolQuery::Dynamic(), "stats");
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
      if (key == "reattached_members") {
        chi::u32 v = 0;
        kv.val.convert(v);
        return static_cast<long>(v);
      }
    }
  }
  return -1;
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
  std::vector<chi::PoolId> data_ids;
  for (int c = 0; c < k; ++c) {
    chi::PoolId id(static_cast<chi::u32>(9600 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }

  // --- Phase 1: create safe_bdev pool X over the fresh members. ---
  chi::PoolId safe_id(static_cast<chi::u32>(9650 + pidsalt), 0);
  clio::run::safe_bdev::Client safe(safe_id);
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }
  {
    auto create_task = safe.AsyncCreate(chi::PoolQuery::Dynamic(),
                                        "safe_bdev_sb_poolX", safe_id,
                                        /*max_failures=*/1, members);
    create_task.Wait();
    safe.pool_id_ = create_task->new_pool_id_;
    REQUIRE(create_task->GetReturnCode() == 0);
  }

  // Fresh members must NOT be reattached.
  REQUIRE(QueryReattachedMembers(safe) == 0);

  // Write + flush + read a stripe so the members hold real data.
  const chi::u64 io_len = static_cast<chi::u64>(k) * kShardLen;
  auto alloc = safe.AsyncAllocateBlocks(chi::PoolQuery::Dynamic(), io_len);
  alloc.Wait();
  REQUIRE(alloc->GetReturnCode() == 0);
  REQUIRE(alloc->blocks_.size() > 0);
  clio::run::bdev::Block block = alloc->blocks_[0];
  std::vector<ctp::u8> pattern = MakePattern(io_len, 0x77);

  {
    chi::priv::vector<clio::run::bdev::Block> wblocks(CTP_MALLOC);
    wblocks.push_back(block);
    auto wbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(wbuf.IsNull());
    memcpy(wbuf.ptr_, pattern.data(), io_len);
    auto wt = safe.AsyncWrite(chi::PoolQuery::Dynamic(), wblocks,
                              wbuf.shm_.template Cast<void>(), io_len);
    wt.Wait();
    REQUIRE(wt->GetReturnCode() == 0);
    REQUIRE(wt->bytes_written_ == io_len);
    CLIO_IPC->FreeBuffer(wbuf);
  }
  {
    auto flush = safe.AsyncBuildParity(chi::PoolQuery::Dynamic(), 0);
    flush.Wait();
    REQUIRE(flush->GetReturnCode() == 0);
  }
  {
    chi::priv::vector<clio::run::bdev::Block> rblocks(CTP_MALLOC);
    rblocks.push_back(block);
    auto rbuf = CLIO_IPC->AllocateBuffer(io_len);
    REQUIRE_FALSE(rbuf.IsNull());
    memset(rbuf.ptr_, 0, io_len);
    auto rt = safe.AsyncRead(chi::PoolQuery::Dynamic(), rblocks,
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
  //     bdev pools). A single daemon's get-or-create is keyed by name/id, so a
  //     plain re-Create would short-circuit to the existing container without
  //     re-running Create; destroying first forces a genuine re-attach that
  //     re-reads each member's superblock. ---
  {
    clio::run::admin::Client admin(chi::kAdminPoolId);
    auto destroy = admin.AsyncDestroyPool(chi::PoolQuery::Dynamic(),
                                          safe.pool_id_);
    destroy.Wait();
    REQUIRE(destroy->GetReturnCode() == 0);
    std::this_thread::sleep_for(100ms);
  }

  // --- Phase 2: create AGAIN with the SAME pool id X over the SAME members.
  //     Every member already carries our superblock => all re-attached. ---
  chi::PoolId safe_id2 = safe_id;  // same identity
  clio::run::safe_bdev::Client safe2(safe_id2);
  {
    auto create_task = safe2.AsyncCreate(chi::PoolQuery::Dynamic(),
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
  std::vector<chi::PoolId> data_ids;
  for (int c = 0; c < k; ++c) {
    chi::PoolId id(static_cast<chi::u32>(9700 + pidsalt + c), 0);
    clio::run::bdev::Client client(id);
    REQUIRE(CreateRamMember(client, member_name(c), id));
    data_ids.push_back(client.pool_id_);
  }
  std::vector<clio::run::safe_bdev::MemberBdevDesc> members;
  for (int c = 0; c < k; ++c) {
    members.emplace_back(member_name(c), /*node_id=*/0, data_ids[c]);
  }

  // --- Array X claims the members (writes superblocks). ---
  chi::PoolId safe_idX(static_cast<chi::u32>(9750 + pidsalt), 0);
  clio::run::safe_bdev::Client safeX(safe_idX);
  {
    auto create_task = safeX.AsyncCreate(chi::PoolQuery::Dynamic(),
                                         "safe_bdev_fr_poolX", safe_idX,
                                         /*max_failures=*/1, members);
    create_task.Wait();
    safeX.pool_id_ = create_task->new_pool_id_;
    REQUIRE(create_task->GetReturnCode() == 0);
  }

  // --- Array Y (DIFFERENT pool id) over the SAME members => must REFUSE. ---
  chi::PoolId safe_idY(static_cast<chi::u32>(9760 + pidsalt), 0);
  clio::run::safe_bdev::Client safeY(safe_idY);
  {
    auto create_task = safeY.AsyncCreate(chi::PoolQuery::Dynamic(),
                                         "safe_bdev_fr_poolY", safe_idY,
                                         /*max_failures=*/1, members);
    create_task.Wait();
    HLOG(kInfo, "safe_bdev FR: foreign create returned rc={}",
         create_task->GetReturnCode());
    REQUIRE(create_task->GetReturnCode() != 0);
  }
  HLOG(kInfo, "safe_bdev superblock foreign-refuse test PASSED");
}

SIMPLE_TEST_MAIN()

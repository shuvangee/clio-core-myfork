/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved. BSD 3-Clause license.
 */

/**
 * Distributed CTE filesystem (CFS) correctness test (issue #685).
 *
 * Verifies that the CFS namespace is SHARED across a multi-node runtime: a file
 * created through the CFS client on one node must be visible and readable, with
 * identical bytes, through the CFS client on a DIFFERENT node. With the pre-#685
 * bug (every tag op issued with PoolQuery::Local()) a path's tag lives only in
 * the issuing node's local container, so the reader on node B gets ENOENT.
 *
 * This is a single binary run in two roles inside a docker cluster (see the
 * docker-compose in this directory), selected by CFS_DIST_ROLE:
 *
 *   writer  (node 1): CLIO_INIT(kClient) -> connects to the LOCAL daemon,
 *                     opens+writes a deterministic pattern to a shared path,
 *                     verifies its own read-back, then stays alive (so its
 *                     container/owner survives) for CFS_KEEPALIVE_SEC.
 *   reader  (node 2): CLIO_INIT(kClient) -> connects to the LOCAL daemon, waits
 *                     CFS_READ_DELAY_SEC for the writer, then retries opening the
 *                     SAME path and verifies the bytes match. ENOENT after the
 *                     retry window == the cross-node namespace is broken (#685).
 *
 * Cross-node ordering/liveness is handled WITHOUT any CFS or shared-fs
 * coordination (so it is independent of the very thing under test): the reader
 * simply delays a few seconds (CFS_READ_DELAY_SEC) so the writer has created the
 * file, then retries the open for a bounded window; the writer, after writing,
 * stays alive (CFS_KEEPALIVE_SEC) so its node's daemon — and thus the container
 * that owns the path — survives for the whole of the reader's read window.
 *
 * Exit code 0 == the role's checks passed. The reader's exit code is the
 * authoritative cross-node correctness signal (the run script keys on it).
 */
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <clio_runtime/clio_runtime.h>
#include <clio_cte/filesystem/filesystem_client.h>

namespace {

// Shared, deterministic file contents so the reader can prove it read the
// WRITER's bytes (not zeros/garbage). 384 KiB spans a sub-page tail on the
// 1 MiB CFS page size, exercising the multi-blob read/write path a little.
constexpr clio::run::u64 kDataSize = 384u * 1024u;
constexpr const char *kSharedPath = "/dist685/shared.bin";
// The CFS filesystem pool composed in clio_config.yaml (kCfsPoolId = 560.0).
constexpr clio::run::u32 kCfsPoolMajor = 560;

// Read an integer env var (seconds) with a default.
int EnvSecs(const char *name, int def) {
  const char *e = std::getenv(name);
  if (!e || !*e) return def;
  int v = std::atoi(e);
  return v > 0 ? v : def;
}

// Byte i of the shared pattern (independent of node, deterministic).
unsigned char PatternByte(clio::run::u64 i) {
  return static_cast<unsigned char>((i * 131u + 7u) & 0xFFu);
}

void Log(const char *role, const std::string &msg) {
  std::fprintf(stderr, "[cfs-dist %s] %s\n", role, msg.c_str());
  std::fflush(stderr);
}

// ---- writer (node A): create + write + verify local read-back --------------
int RunWriter() {
  const char *role = "writer";
  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, false)) {
    Log(role, "FAIL: CLIO_INIT(kClient) failed");
    return 2;
  }
  clio::cte::filesystem::Client cfs;
  cfs.Init(clio::run::PoolId(kCfsPoolMajor, 0));
  auto *ipc = CLIO_IPC;

  auto open = cfs.AsyncOpen(kSharedPath, O_CREAT | O_RDWR, 0644);
  open.Wait();
  if (open->GetReturnCode() != 0 || open->handle_ == 0) {
    Log(role, "FAIL: AsyncOpen(create) rc=" +
                  std::to_string(open->GetReturnCode()));
    return 3;
  }
  clio::run::u64 h = open->handle_;

  ctp::ipc::FullPtr<char> wbuf = ipc->AllocateBuffer(kDataSize);
  for (clio::run::u64 i = 0; i < kDataSize; ++i) wbuf.ptr_[i] = PatternByte(i);
  auto w = cfs.AsyncWrite(h, 0, kDataSize, wbuf.shm_.template Cast<void>());
  w.Wait();
  if (w->GetReturnCode() != 0) {
    Log(role, "FAIL: AsyncWrite rc=" + std::to_string(w->GetReturnCode()));
    return 4;
  }

  // Local read-back sanity on the writer node (same-node routing must already
  // work; this isolates cross-node failures from local ones).
  ctp::ipc::FullPtr<char> rbuf = ipc->AllocateBuffer(kDataSize);
  std::memset(rbuf.ptr_, 0, kDataSize);
  auto r = cfs.AsyncRead(h, 0, kDataSize, rbuf.shm_.template Cast<void>());
  r.Wait();
  if (r->GetReturnCode() != 0 || r->bytes_read_ != kDataSize) {
    Log(role, "FAIL: local read-back rc=" + std::to_string(r->GetReturnCode()) +
                  " bytes=" + std::to_string(r->bytes_read_));
    return 5;
  }
  for (clio::run::u64 i = 0; i < kDataSize; ++i) {
    if (static_cast<unsigned char>(rbuf.ptr_[i]) != PatternByte(i)) {
      Log(role, "FAIL: local read-back mismatch at " + std::to_string(i));
      return 6;
    }
  }
  auto cl = cfs.AsyncClose(h);
  cl.Wait();
  int keepalive = EnvSecs("CFS_KEEPALIVE_SEC", 75);
  Log(role, "wrote+verified " + std::to_string(kDataSize) + " bytes to " +
                kSharedPath + "; staying alive " + std::to_string(keepalive) +
                "s for the reader");

  // Keep this node's daemon (and thus the container that owns the path) alive
  // for the whole of the reader's read window, so a cross-node read from the
  // other node still resolves. Independent of the CFS being tested.
  std::this_thread::sleep_for(std::chrono::seconds(keepalive));
  Log(role, "keep-alive elapsed; exiting");
  return 0;  // the writer's own operations succeeded
}

// ---- reader (node B): must SEE the writer's file across the cluster --------
int RunReader() {
  const char *role = "reader";
  // Give the writer time to create+write the file before we start looking (the
  // retry loop below tolerates the rest of the skew).
  int delay = EnvSecs("CFS_READ_DELAY_SEC", 12);
  Log(role, "delaying " + std::to_string(delay) + "s for the writer, then reading");
  std::this_thread::sleep_for(std::chrono::seconds(delay));
  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, false)) {
    Log(role, "FAIL: CLIO_INIT(kClient) failed");
    return 3;
  }
  clio::cte::filesystem::Client cfs;
  cfs.Init(clio::run::PoolId(kCfsPoolMajor, 0));
  auto *ipc = CLIO_IPC;

  // Retry the cross-node open: the shared namespace should resolve the tag the
  // writer created (on whichever container owns hash(path)) from THIS node too.
  int rc = 1;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
  clio::run::u64 last_rc = 999, last_bytes = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    auto open = cfs.AsyncOpen(kSharedPath, O_RDONLY, 0644);
    open.Wait();
    // handle_==0 means "not found" (the adapters map that to ENOENT) — the #685
    // symptom on a broken (node-local) namespace.
    if (open->GetReturnCode() == 0 && open->handle_ != 0 &&
        open->size_ >= kDataSize) {
      clio::run::u64 h = open->handle_;
      ctp::ipc::FullPtr<char> rbuf = ipc->AllocateBuffer(kDataSize);
      std::memset(rbuf.ptr_, 0, kDataSize);
      auto r = cfs.AsyncRead(h, 0, kDataSize, rbuf.shm_.template Cast<void>());
      r.Wait();
      auto cl = cfs.AsyncClose(h);
      cl.Wait();
      last_rc = r->GetReturnCode();
      last_bytes = r->bytes_read_;
      if (r->GetReturnCode() == 0 && r->bytes_read_ == kDataSize) {
        bool match = true;
        for (clio::run::u64 i = 0; i < kDataSize; ++i) {
          if (static_cast<unsigned char>(rbuf.ptr_[i]) != PatternByte(i)) {
            match = false;
            Log(role, "FAIL: cross-node byte mismatch at " + std::to_string(i));
            break;
          }
        }
        if (match) {
          Log(role, "PASS: cross-node read of " + std::to_string(kDataSize) +
                        " bytes from " + kSharedPath + " matched the writer");
          rc = 0;
        }
        break;  // found + read (pass or data-mismatch): done retrying
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  if (rc != 0) {
    Log(role, "FAIL: could not read the writer's file cross-node "
              "(ENOENT/short read) — CFS namespace is node-local (#685). "
              "last read rc=" + std::to_string(last_rc) +
              " bytes=" + std::to_string(last_bytes));
  }
  return rc;
}

}  // namespace

int main() {
  const char *role = std::getenv("CFS_DIST_ROLE");
  if (!role) {
    std::fprintf(stderr, "CFS_DIST_ROLE not set (expected 'writer' or 'reader')\n");
    return 64;
  }
  if (std::strcmp(role, "writer") == 0) return RunWriter();
  if (std::strcmp(role, "reader") == 0) return RunReader();
  std::fprintf(stderr, "unknown CFS_DIST_ROLE='%s'\n", role);
  return 64;
}

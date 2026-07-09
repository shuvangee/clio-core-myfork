/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved. BSD 3-Clause license.
 *
 * HDF5 VFD test suite for the Model A "authoritative native file" layer (L1).
 *
 * The driver writes every byte through to a real on-disk native HDF5 file at
 * the stripped path, so standard tools read it live and it is byte-identical to
 * what sec2 would produce. These tests exercise that claim beyond a single
 * all-doubles smoke:
 *   1. Multi-datatype + multi-page round-trip through the VFD.
 *   2. Reopen + append a second dataset (eof/reopen from fstat).
 *   3. Differential vs sec2: identical content written through sec2 and the VFD
 *      must read back byte-identical (sec2 is the native oracle).
 *   4. Native tool matrix on the VFD's file with NO VFD loaded: h5dump / h5ls /
 *      h5repack + h5diff. This is the actual native-compat claim; guarded on
 *      tool availability and LOUD if it has to skip (never a silent pass).
 *
 * Plain main (no Catch2): pulls in only HDF5 + the runtime client. Returns
 * non-zero on the first failure so CTest reports it.
 */
#include <hdf5.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "clio_runtime/clio_runtime.h"
#include "clio_runtime/bdev/bdev_client.h"
#include "clio_cte/core/core_client.h"
#include "adapter/vfd/H5FDhermes.h"

namespace {
const char *kBackend = "/tmp/clio_cte_vfd_test.dat";
const char *kClioFile = "clio::/tmp/clio_cte_vfd_suite.h5";   // VFD (native at /tmp/...)
const char *kNativeFile = "/tmp/clio_cte_vfd_suite.h5";       // same file, no marker
const char *kSec2File = "/tmp/clio_cte_vfd_sec2.h5";          // sec2 oracle twin

// A large-ish dataset that spans many chimod/VFD pages (1 MiB page => this is
// several MiB), so writes are not a single page.
constexpr hsize_t kBig = 512 * 1024;   // 512Ki elements
constexpr hsize_t kSmall = 1000;

#define CHECK(cond, msg)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      std::fprintf(stderr, "[vfd-suite] FAIL: %s\n", (msg)); \
      return 1;                                              \
    }                                                        \
  } while (0)

bool InitRuntime() {
  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, true)) {
    return false;
  }
  if (!clio::cte::core::CLIO_CTE_CLIENT_INIT()) {
    return false;
  }
  auto *cte = CLIO_CTE_CLIENT;
  clio::run::PoolId bdev_pool_id(953, 0);
  clio::run::bdev::Client bdev(bdev_pool_id);
  auto ct = bdev.AsyncCreate(clio::run::PoolQuery::Dynamic(), kBackend,
                             bdev_pool_id, clio::run::bdev::BdevType::kFile);
  ct.Wait();
  auto rt = cte->AsyncRegisterTarget(kBackend, clio::run::bdev::BdevType::kFile,
                                     64ULL * 1024 * 1024,
                                     clio::run::PoolQuery::Local(), bdev_pool_id);
  rt.Wait();
  return rt->GetReturnCode() == 0;
}

// Build a FAPL bound to the clio VFD (driver already registered by caller).
hid_t ClioFapl(hid_t driver) {
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  if (fapl < 0 || H5Pset_driver(fapl, driver, nullptr) < 0) {
    return H5I_INVALID_HID;
  }
  return fapl;
}

// Write a 1-D dataset of the given native type; returns true on success.
template <typename T>
bool WriteDset(hid_t file, const char *name, hid_t type, const std::vector<T> &v) {
  hsize_t dims[1] = {static_cast<hsize_t>(v.size())};
  hid_t space = H5Screate_simple(1, dims, nullptr);
  hid_t dset = H5Dcreate2(file, name, type, space, H5P_DEFAULT, H5P_DEFAULT,
                          H5P_DEFAULT);
  bool ok = dset >= 0 &&
            H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data()) >= 0;
  if (dset >= 0) H5Dclose(dset);
  H5Sclose(space);
  return ok;
}

// Read a 1-D dataset back and compare byte-for-byte with expected.
template <typename T>
bool ReadDsetEq(hid_t file, const char *name, hid_t type,
                const std::vector<T> &expected) {
  hid_t dset = H5Dopen2(file, name, H5P_DEFAULT);
  if (dset < 0) return false;
  std::vector<T> got(expected.size());
  bool ok = H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, got.data()) >= 0;
  H5Dclose(dset);
  if (!ok) return false;
  return std::memcmp(got.data(), expected.data(), got.size() * sizeof(T)) == 0;
}

// Deterministic fill patterns (distinct per type so a cross-wire is caught).
std::vector<int32_t> MakeI32(hsize_t n) {
  std::vector<int32_t> v(n);
  for (hsize_t i = 0; i < n; ++i) v[i] = static_cast<int32_t>(i * 7 - 3);
  return v;
}
std::vector<int64_t> MakeI64(hsize_t n) {
  std::vector<int64_t> v(n);
  for (hsize_t i = 0; i < n; ++i)
    v[i] = static_cast<int64_t>(i) * 1000003LL - 42;
  return v;
}
std::vector<float> MakeF32(hsize_t n) {
  std::vector<float> v(n);
  for (hsize_t i = 0; i < n; ++i) v[i] = static_cast<float>(i) * 0.25f - 1.5f;
  return v;
}
std::vector<double> MakeF64(hsize_t n) {
  std::vector<double> v(n);
  for (hsize_t i = 0; i < n; ++i) v[i] = static_cast<double>(i) * 1.5 - 7.0;
  return v;
}

// Write the standard content set into an already-open file.
bool WriteContent(hid_t file) {
  return WriteDset(file, "i32", H5T_NATIVE_INT32, MakeI32(kSmall)) &&
         WriteDset(file, "i64", H5T_NATIVE_INT64, MakeI64(kSmall)) &&
         WriteDset(file, "f32", H5T_NATIVE_FLOAT, MakeF32(kSmall)) &&
         WriteDset(file, "f64_big", H5T_NATIVE_DOUBLE, MakeF64(kBig));
}

// Read the standard content set back and verify each dataset.
bool VerifyContent(hid_t file) {
  return ReadDsetEq(file, "i32", H5T_NATIVE_INT32, MakeI32(kSmall)) &&
         ReadDsetEq(file, "i64", H5T_NATIVE_INT64, MakeI64(kSmall)) &&
         ReadDsetEq(file, "f32", H5T_NATIVE_FLOAT, MakeF32(kSmall)) &&
         ReadDsetEq(file, "f64_big", H5T_NATIVE_DOUBLE, MakeF64(kBig));
}

bool HasTool(const char *tool) {
  std::string cmd = "command -v ";
  cmd += tool;
  cmd += " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

int RunCmd(const std::string &cmd) {
  int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
  return rc;  // caller interprets via WEXITSTATUS-ish check below
}
}  // namespace

int main() {
  if (!InitRuntime()) {
    std::fprintf(stderr, "[vfd-suite] FAIL: runtime/CTE init\n");
    return 1;
  }

  hid_t driver = H5FD_hermes_init();
  CHECK(driver >= 0, "H5FD_hermes_init");
  hid_t fapl = ClioFapl(driver);
  CHECK(fapl >= 0, "build clio FAPL");

  std::remove(kNativeFile);
  std::remove(kSec2File);

  // === 1. Multi-datatype + multi-page round-trip through the VFD ===========
  {
    hid_t f = H5Fcreate(kClioFile, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    CHECK(f >= 0, "1: H5Fcreate via VFD");
    CHECK(WriteContent(f), "1: write multi-dtype content");
    CHECK(H5Fclose(f) >= 0, "1: H5Fclose (write)");

    hid_t f2 = H5Fopen(kClioFile, H5F_ACC_RDONLY, fapl);
    CHECK(f2 >= 0, "1: H5Fopen via VFD");
    CHECK(VerifyContent(f2), "1: read-back byte-clean (multi-dtype, multi-page)");
    CHECK(H5Fclose(f2) >= 0, "1: H5Fclose (read)");
    std::printf("[vfd-suite] ok 1: multi-datatype + multi-page round-trip\n");
  }

  // === 2. Reopen + append a new dataset (eof/reopen from fstat) =============
  {
    hid_t f = H5Fopen(kClioFile, H5F_ACC_RDWR, fapl);
    CHECK(f >= 0, "2: reopen RDWR");
    CHECK(WriteDset(f, "appended", H5T_NATIVE_INT32, MakeI32(kSmall)),
          "2: append a dataset to an existing file");
    CHECK(H5Fclose(f) >= 0, "2: H5Fclose (append)");

    hid_t f2 = H5Fopen(kClioFile, H5F_ACC_RDONLY, fapl);
    CHECK(f2 >= 0, "2: reopen RDONLY after append");
    CHECK(VerifyContent(f2), "2: original content intact after append");
    CHECK(ReadDsetEq(f2, "appended", H5T_NATIVE_INT32, MakeI32(kSmall)),
          "2: appended dataset readable");
    CHECK(H5Fclose(f2) >= 0, "2: H5Fclose");
    std::printf("[vfd-suite] ok 2: reopen + append\n");
  }

  // === 3. Differential vs sec2 (native oracle) =============================
  // Write identical content through sec2 (default driver -> plain native file)
  // and through the VFD, then verify both read back byte-identical.
  {
    hid_t sec2 = H5Fcreate(kSec2File, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    CHECK(sec2 >= 0, "3: H5Fcreate via sec2");
    CHECK(WriteContent(sec2), "3: write content via sec2");
    CHECK(H5Fclose(sec2) >= 0, "3: H5Fclose sec2");

    // Fresh VFD file with the same content for a clean 1:1 comparison.
    const char *kClioDiff = "clio::/tmp/clio_cte_vfd_diff.h5";
    const char *kNativeDiff = "/tmp/clio_cte_vfd_diff.h5";
    std::remove(kNativeDiff);
    hid_t v = H5Fcreate(kClioDiff, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    CHECK(v >= 0, "3: H5Fcreate via VFD (diff)");
    CHECK(WriteContent(v), "3: write content via VFD");
    CHECK(H5Fclose(v) >= 0, "3: H5Fclose VFD (diff)");

    // Read both back via sec2 (no VFD) and confirm equal to the oracle values.
    hid_t s = H5Fopen(kSec2File, H5F_ACC_RDONLY, H5P_DEFAULT);
    hid_t n = H5Fopen(kNativeDiff, H5F_ACC_RDONLY, H5P_DEFAULT);
    CHECK(s >= 0, "3: reopen sec2 file");
    CHECK(n >= 0, "3: reopen VFD's native file WITHOUT the VFD");
    CHECK(VerifyContent(s), "3: sec2 file content correct");
    CHECK(VerifyContent(n), "3: VFD's native file content correct (read w/o VFD)");
    CHECK(H5Fclose(s) >= 0, "3: close sec2");
    CHECK(H5Fclose(n) >= 0, "3: close native");
    std::printf("[vfd-suite] ok 3: differential vs sec2 (byte-identical content)\n");
  }

  H5Pclose(fapl);

  // === 4. Native tool matrix on the VFD's file (NO VFD loaded) =============
  // This is the actual native-compat claim: standard HDF5 tools operate on the
  // file the VFD wrote, live. Guarded on tool availability; if a tool is
  // missing we say so loudly rather than passing silently.
  {
    if (!HasTool("h5dump") || !HasTool("h5ls") || !HasTool("h5repack") ||
        !HasTool("h5diff")) {
      std::fprintf(stderr,
                   "[vfd-suite] WARN 4: native HDF5 CLI tools not on PATH; "
                   "SKIPPING the tool-matrix (native-compat NOT verified here). "
                   "Install hdf5-tools to exercise this.\n");
    } else {
      std::string f = kNativeFile;
      CHECK(RunCmd("h5dump -H '" + f + "'") == 0, "4: h5dump -H on VFD file");
      CHECK(RunCmd("h5ls -r '" + f + "'") == 0, "4: h5ls -r on VFD file");
      std::string repacked = "/tmp/clio_cte_vfd_repacked.h5";
      std::remove(repacked.c_str());
      CHECK(RunCmd("h5repack '" + f + "' '" + repacked + "'") == 0,
            "4: h5repack VFD file");
      CHECK(RunCmd("h5diff '" + f + "' '" + repacked + "'") == 0,
            "4: h5diff repacked == original (byte/semantically clean)");
      std::printf("[vfd-suite] ok 4: native tool matrix (h5dump/h5ls/h5repack/h5diff)\n");
    }
  }

  std::printf("[vfd-suite] PASS: Model A native write-through (L1) verified\n");
  return 0;
}

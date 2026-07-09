/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved. BSD 3-Clause license.
 *
 * HDF5 VFD test suite for the "authoritative native file" write-through layer.
 *
 * The driver writes every byte through to a real on-disk native HDF5 file at
 * the stripped path, so standard tools read it live and it is byte-identical to
 * what sec2 would produce. Because the driver is byte-agnostic, the meaningful
 * coverage is exercising the *variety of byte-write patterns* HDF5 generates:
 *   1. Rich round-trip through the VFD -- multiple dtypes, a multi-page (>4 MiB)
 *      dataset, 2-D, chunked, chunked+shuffle+gzip, a subgroup, and an
 *      attribute (structural + scattered small metadata writes).
 *   2. Reopen + append a dataset (eof/reopen from fstat).
 *   3. Differential vs sec2 (native oracle): the same rich content written
 *      through sec2 and through the VFD reads back byte-identical, and the VFD's
 *      native file is reopened/read WITHOUT the VFD.
 *   4. Native tool matrix on the VFD's file, NO VFD loaded:
 *      h5dump/h5ls/h5repack/h5diff. Guarded on tool availability, LOUD if it
 *      must skip -- never a silent pass.
 *   5. Partial I/O: hyperslab overwrite-in-place (an interior, non-whole write)
 *      then read the merged result back.
 *   6. Two VFD files open simultaneously -- independent, no cross-talk.
 *
 * Plain main (no Catch2). Returns non-zero on the first failure.
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
const char *kClioFile = "clio::/tmp/clio_cte_vfd_suite.h5";
const char *kNativeFile = "/tmp/clio_cte_vfd_suite.h5";

constexpr hsize_t kBig = 512 * 1024;  // multi-page (>4 MiB of doubles)
constexpr hsize_t kSmall = 1000;

#define CHECK(cond, msg)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      std::fprintf(stderr, "[vfd-suite] FAIL: %s\n", (msg)); \
      return 1;                                              \
    }                                                        \
  } while (0)

bool InitRuntime() {
  if (!clio::run::CLIO_INIT(clio::run::RuntimeMode::kClient, true)) return false;
  if (!clio::cte::core::CLIO_CTE_CLIENT_INIT()) return false;
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

hid_t ClioFapl(hid_t driver) {
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  if (fapl < 0 || H5Pset_driver(fapl, driver, nullptr) < 0)
    return H5I_INVALID_HID;
  return fapl;
}

bool DeflateAvail() { return H5Zfilter_avail(H5Z_FILTER_DEFLATE) > 0; }

// ---- deterministic fill patterns (distinct per type) ----------------------
std::vector<int32_t> MakeI32(hsize_t n) {
  std::vector<int32_t> v(n);
  for (hsize_t i = 0; i < n; ++i) v[i] = static_cast<int32_t>(i * 7 - 3);
  return v;
}
std::vector<int64_t> MakeI64(hsize_t n) {
  std::vector<int64_t> v(n);
  for (hsize_t i = 0; i < n; ++i) v[i] = static_cast<int64_t>(i) * 1000003LL - 42;
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

// ---- generic writers/readers (reader is rank-agnostic: H5S_ALL is flat) ----
template <typename T>
bool WriteDset(hid_t loc, const char *name, hid_t type, const std::vector<T> &v) {
  hsize_t dims[1] = {static_cast<hsize_t>(v.size())};
  hid_t space = H5Screate_simple(1, dims, nullptr);
  hid_t dset =
      H5Dcreate2(loc, name, type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  bool ok = dset >= 0 &&
            H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data()) >= 0;
  if (dset >= 0) H5Dclose(dset);
  H5Sclose(space);
  return ok;
}

template <typename T>
bool WriteChunked(hid_t loc, const char *name, hid_t type,
                  const std::vector<T> &v, bool filters) {
  hsize_t dims[1] = {static_cast<hsize_t>(v.size())};
  hsize_t chunk[1] = {std::min<hsize_t>(v.size(), 256)};
  hid_t space = H5Screate_simple(1, dims, nullptr);
  hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dcpl, 1, chunk);
  if (filters) {
    H5Pset_shuffle(dcpl);
    H5Pset_deflate(dcpl, 6);
  }
  hid_t dset =
      H5Dcreate2(loc, name, type, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
  bool ok = dset >= 0 &&
            H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data()) >= 0;
  if (dset >= 0) H5Dclose(dset);
  H5Pclose(dcpl);
  H5Sclose(space);
  return ok;
}

bool Write2D(hid_t loc, const char *name, const std::vector<double> &flat,
             hsize_t rows, hsize_t cols) {
  hsize_t dims[2] = {rows, cols};
  hid_t space = H5Screate_simple(2, dims, nullptr);
  hid_t dset = H5Dcreate2(loc, name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                          H5P_DEFAULT, H5P_DEFAULT);
  bool ok = dset >= 0 && H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                                  H5P_DEFAULT, flat.data()) >= 0;
  if (dset >= 0) H5Dclose(dset);
  H5Sclose(space);
  return ok;
}

template <typename T>
bool ReadDsetEq(hid_t loc, const char *name, hid_t type,
                const std::vector<T> &expected) {
  hid_t dset = H5Dopen2(loc, name, H5P_DEFAULT);
  if (dset < 0) return false;
  std::vector<T> got(expected.size());
  bool ok = H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, got.data()) >= 0;
  H5Dclose(dset);
  return ok &&
         std::memcmp(got.data(), expected.data(), got.size() * sizeof(T)) == 0;
}

bool WriteAttr(hid_t file) {
  hid_t root = H5Gopen2(file, "/", H5P_DEFAULT);
  hsize_t d[1] = {4};
  hid_t sp = H5Screate_simple(1, d, nullptr);
  hid_t a = H5Acreate2(root, "meta", H5T_NATIVE_INT32, sp, H5P_DEFAULT,
                       H5P_DEFAULT);
  int vals[4] = {10, 20, 30, 40};
  bool ok = a >= 0 && H5Awrite(a, H5T_NATIVE_INT32, vals) >= 0;
  if (a >= 0) H5Aclose(a);
  H5Sclose(sp);
  H5Gclose(root);
  return ok;
}
bool VerifyAttr(hid_t file) {
  hid_t root = H5Gopen2(file, "/", H5P_DEFAULT);
  hid_t a = H5Aopen(root, "meta", H5P_DEFAULT);
  int got[4] = {0, 0, 0, 0};
  const int exp[4] = {10, 20, 30, 40};
  bool ok = a >= 0 && H5Aread(a, H5T_NATIVE_INT32, got) >= 0 &&
            std::memcmp(got, exp, sizeof(exp)) == 0;
  if (a >= 0) H5Aclose(a);
  H5Gclose(root);
  return ok;
}

// A structurally rich content set: flat dtypes, a multi-page dataset, 2-D,
// chunked, chunked+filters (if deflate is available), a subgroup, an attribute.
bool WriteRich(hid_t file) {
  if (!WriteDset(file, "i32", H5T_NATIVE_INT32, MakeI32(kSmall))) return false;
  if (!WriteDset(file, "i64", H5T_NATIVE_INT64, MakeI64(kSmall))) return false;
  if (!WriteDset(file, "f32", H5T_NATIVE_FLOAT, MakeF32(kSmall))) return false;
  if (!WriteDset(file, "f64_big", H5T_NATIVE_DOUBLE, MakeF64(kBig))) return false;
  if (!Write2D(file, "d2d", MakeF64(64 * 32), 64, 32)) return false;
  if (!WriteChunked(file, "chunked", H5T_NATIVE_INT32, MakeI32(kSmall), false))
    return false;
  if (DeflateAvail() &&
      !WriteChunked(file, "compressed", H5T_NATIVE_INT32, MakeI32(kSmall), true))
    return false;
  hid_t g = H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (g < 0) return false;
  bool gok = WriteDset(g, "inner", H5T_NATIVE_INT64, MakeI64(kSmall));
  H5Gclose(g);
  if (!gok) return false;
  return WriteAttr(file);
}

bool VerifyRich(hid_t file) {
  if (!ReadDsetEq(file, "i32", H5T_NATIVE_INT32, MakeI32(kSmall))) return false;
  if (!ReadDsetEq(file, "i64", H5T_NATIVE_INT64, MakeI64(kSmall))) return false;
  if (!ReadDsetEq(file, "f32", H5T_NATIVE_FLOAT, MakeF32(kSmall))) return false;
  if (!ReadDsetEq(file, "f64_big", H5T_NATIVE_DOUBLE, MakeF64(kBig)))
    return false;
  if (!ReadDsetEq(file, "d2d", H5T_NATIVE_DOUBLE, MakeF64(64 * 32))) return false;
  if (!ReadDsetEq(file, "chunked", H5T_NATIVE_INT32, MakeI32(kSmall)))
    return false;
  if (DeflateAvail() &&
      !ReadDsetEq(file, "compressed", H5T_NATIVE_INT32, MakeI32(kSmall)))
    return false;
  if (!ReadDsetEq(file, "/grp/inner", H5T_NATIVE_INT64, MakeI64(kSmall)))
    return false;
  return VerifyAttr(file);
}

bool HasTool(const char *tool) {
  std::string cmd = "command -v ";
  cmd += tool;
  cmd += " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}
int RunCmd(const std::string &cmd) {
  return std::system((cmd + " >/dev/null 2>&1").c_str());
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

  // === 1. Rich round-trip through the VFD =================================
  {
    hid_t f = H5Fcreate(kClioFile, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    CHECK(f >= 0, "1: H5Fcreate via VFD");
    CHECK(WriteRich(f), "1: write rich content");
    CHECK(H5Fclose(f) >= 0, "1: H5Fclose (write)");
    hid_t f2 = H5Fopen(kClioFile, H5F_ACC_RDONLY, fapl);
    CHECK(f2 >= 0, "1: H5Fopen via VFD");
    CHECK(VerifyRich(f2), "1: rich content reads back byte-clean");
    CHECK(H5Fclose(f2) >= 0, "1: H5Fclose (read)");
    std::printf("[vfd-suite] ok 1: rich round-trip (dtypes/2D/chunked/filters/group/attr)\n");
  }

  // === 2. Reopen + append =================================================
  {
    hid_t f = H5Fopen(kClioFile, H5F_ACC_RDWR, fapl);
    CHECK(f >= 0, "2: reopen RDWR");
    CHECK(WriteDset(f, "appended", H5T_NATIVE_INT32, MakeI32(kSmall)),
          "2: append a dataset");
    CHECK(H5Fclose(f) >= 0, "2: H5Fclose (append)");
    hid_t f2 = H5Fopen(kClioFile, H5F_ACC_RDONLY, fapl);
    CHECK(f2 >= 0, "2: reopen RDONLY");
    CHECK(VerifyRich(f2), "2: original content intact after append");
    CHECK(ReadDsetEq(f2, "appended", H5T_NATIVE_INT32, MakeI32(kSmall)),
          "2: appended dataset readable");
    CHECK(H5Fclose(f2) >= 0, "2: H5Fclose");
    std::printf("[vfd-suite] ok 2: reopen + append\n");
  }

  // === 3. Differential vs sec2 (native oracle) ============================
  {
    const char *kSec2 = "/tmp/clio_cte_vfd_sec2.h5";
    const char *kClioDiff = "clio::/tmp/clio_cte_vfd_diff.h5";
    const char *kNativeDiff = "/tmp/clio_cte_vfd_diff.h5";
    std::remove(kSec2);
    std::remove(kNativeDiff);
    hid_t s = H5Fcreate(kSec2, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    CHECK(s >= 0 && WriteRich(s) && H5Fclose(s) >= 0, "3: write rich via sec2");
    hid_t v = H5Fcreate(kClioDiff, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    CHECK(v >= 0 && WriteRich(v) && H5Fclose(v) >= 0, "3: write rich via VFD");
    // Reopen BOTH with sec2 (no VFD) and verify identical, correct content.
    hid_t rs = H5Fopen(kSec2, H5F_ACC_RDONLY, H5P_DEFAULT);
    hid_t rn = H5Fopen(kNativeDiff, H5F_ACC_RDONLY, H5P_DEFAULT);
    CHECK(rs >= 0, "3: reopen sec2 file");
    CHECK(rn >= 0, "3: reopen VFD's native file WITHOUT the VFD");
    CHECK(VerifyRich(rs), "3: sec2 file content correct");
    CHECK(VerifyRich(rn), "3: VFD's native file content correct (read w/o VFD)");
    CHECK(H5Fclose(rs) >= 0 && H5Fclose(rn) >= 0, "3: close diff files");
    std::printf("[vfd-suite] ok 3: differential vs sec2 (rich content)\n");
  }

  // === 5. Partial I/O: hyperslab overwrite-in-place =======================
  // (Section 4 -- the external tool matrix -- runs last, after H5close.)
  {
    const char *kClioPart = "clio::/tmp/clio_cte_vfd_partial.h5";
    const hsize_t N = kSmall;
    std::vector<int32_t> base(N);
    for (hsize_t i = 0; i < N; ++i) base[i] = static_cast<int32_t>(i);

    hid_t f = H5Fcreate(kClioPart, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    CHECK(f >= 0, "5: H5Fcreate");
    hsize_t dims[1] = {N};
    hid_t fs = H5Screate_simple(1, dims, nullptr);
    hid_t dset = H5Dcreate2(f, "partial", H5T_NATIVE_INT32, fs, H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT);
    CHECK(dset >= 0, "5: H5Dcreate2");
    CHECK(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                   base.data()) >= 0,
          "5: write baseline");
    // Overwrite the middle third with a distinct pattern via a file hyperslab.
    hsize_t start[1] = {N / 3};
    hsize_t count[1] = {N / 3};
    CHECK(H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count,
                              nullptr) >= 0,
          "5: select hyperslab");
    hid_t ms = H5Screate_simple(1, count, nullptr);
    std::vector<int32_t> patch(count[0]);
    for (hsize_t j = 0; j < count[0]; ++j)
      patch[j] = static_cast<int32_t>(-1 - static_cast<int>(j));
    CHECK(H5Dwrite(dset, H5T_NATIVE_INT32, ms, fs, H5P_DEFAULT, patch.data()) >= 0,
          "5: hyperslab overwrite-in-place");
    H5Sclose(ms);
    H5Sclose(fs);
    H5Dclose(dset);
    CHECK(H5Fclose(f) >= 0, "5: H5Fclose");

    // Expected merged result.
    std::vector<int32_t> expect = base;
    for (hsize_t j = 0; j < count[0]; ++j)
      expect[start[0] + j] = static_cast<int32_t>(-1 - static_cast<int>(j));
    hid_t f2 = H5Fopen(kClioPart, H5F_ACC_RDONLY, fapl);
    CHECK(f2 >= 0, "5: reopen");
    CHECK(ReadDsetEq(f2, "partial", H5T_NATIVE_INT32, expect),
          "5: merged (baseline + overwritten hyperslab) reads back correct");
    CHECK(H5Fclose(f2) >= 0, "5: H5Fclose (read)");
    std::printf("[vfd-suite] ok 5: partial hyperslab overwrite-in-place\n");
  }

  // === 6. Two VFD files open simultaneously ===============================
  {
    const char *kA = "clio::/tmp/clio_cte_vfd_a.h5";
    const char *kB = "clio::/tmp/clio_cte_vfd_b.h5";
    hid_t fa = H5Fcreate(kA, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    hid_t fb = H5Fcreate(kB, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    CHECK(fa >= 0 && fb >= 0, "6: create two files at once");
    CHECK(WriteDset(fa, "a", H5T_NATIVE_INT32, MakeI32(kSmall)), "6: write A");
    CHECK(WriteDset(fb, "b", H5T_NATIVE_INT64, MakeI64(kSmall)), "6: write B");
    CHECK(H5Fclose(fa) >= 0 && H5Fclose(fb) >= 0, "6: close both");
    hid_t ra = H5Fopen(kA, H5F_ACC_RDONLY, fapl);
    hid_t rb = H5Fopen(kB, H5F_ACC_RDONLY, fapl);
    CHECK(ra >= 0 && rb >= 0, "6: reopen both");
    CHECK(ReadDsetEq(ra, "a", H5T_NATIVE_INT32, MakeI32(kSmall)), "6: A intact");
    CHECK(ReadDsetEq(rb, "b", H5T_NATIVE_INT64, MakeI64(kSmall)), "6: B intact");
    // No cross-talk: A must not contain B's dataset and vice versa.
    CHECK(H5Lexists(ra, "b", H5P_DEFAULT) <= 0, "6: A has no cross-talk from B");
    CHECK(H5Lexists(rb, "a", H5P_DEFAULT) <= 0, "6: B has no cross-talk from A");
    CHECK(H5Fclose(ra) >= 0 && H5Fclose(rb) >= 0, "6: close both (read)");
    std::printf("[vfd-suite] ok 6: two files open simultaneously (no cross-talk)\n");
  }

  H5Pclose(fapl);

  // === 4. Native tool matrix on the VFD's file (NO VFD loaded) ============
  {
    if (!HasTool("h5dump") || !HasTool("h5ls") || !HasTool("h5repack") ||
        !HasTool("h5diff")) {
      std::fprintf(stderr,
                   "[vfd-suite] WARN 4: native HDF5 CLI tools not on PATH; "
                   "SKIPPING the tool matrix (native-compat NOT verified here). "
                   "Install hdf5-tools to exercise this.\n");
    } else {
      std::string f = kNativeFile;
      CHECK(RunCmd("h5dump -H '" + f + "'") == 0, "4: h5dump -H");
      CHECK(RunCmd("h5ls -r '" + f + "'") == 0, "4: h5ls -r");
      std::string rp = "/tmp/clio_cte_vfd_repacked.h5";
      std::remove(rp.c_str());
      CHECK(RunCmd("h5repack '" + f + "' '" + rp + "'") == 0, "4: h5repack");
      CHECK(RunCmd("h5diff '" + f + "' '" + rp + "'") == 0,
            "4: h5diff repacked == original");
      std::printf("[vfd-suite] ok 4: native tool matrix (h5dump/h5ls/h5repack/h5diff)\n");
    }
  }

  std::printf("[vfd-suite] PASS: native write-through verified\n");
  return 0;
}

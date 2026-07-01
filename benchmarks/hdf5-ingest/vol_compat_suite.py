#!/usr/bin/env python3
"""Phase 1 / Part A — iowarp VOL compatibility suite (differential testing).

Does routing HDF5 through the iowarp VOL preserve native HDF5 semantics? Method:
the NATIVE VOL is the oracle. For each feature case, exercise four arms and assert
the file content (data + metadata) matches:

  native write -> native read   (reference)
  VOL write    -> native read    (write compat: VOL emits a valid native file)
  native write -> VOL read       (read compat)
  VOL write    -> VOL read        (round-trip)

plus h5diff(native_file, vol_file) is clean and h5dump/h5ls succeed on the
VOL-written file. Output: a per-case pass/fail matrix. CI-shaped (nonzero exit on
any failure). Runs INSIDE the clio-core dev container; the driver keeps clio_run
up and spawns each arm as a subprocess with the VOL env toggled.

  python3 vol_compat_suite.py --out vol_compat_results.json
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

BIN = os.environ.get("CLIO_VOL_BIN", "/workspace/build/bin")
CLIO_HOME = os.environ.get("CLIO_HOME_DIR", "/home/iowarp")
RUNTIME_LOG = "/tmp/clio_run_volcompat.log"
RUNTIME_READY = "All 3 pools created successfully"
TMP = "/tmp/volcompat"

# ---------------------------------------------------------------- write fixtures
# Each writer builds a deterministic file at `path` via h5py. VOL on/off is set
# by the driver through the process env (HDF5_VOL_CONNECTOR).

def w_int32_1d_contig(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("a", data=np.arange(4096, dtype="i4"))

def w_float64_2d_chunked(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("m", data=np.linspace(0, 1, 64 * 64, dtype="f8").reshape(64, 64),
                         chunks=(16, 64))

def w_float32_3d_contig(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("v", data=np.arange(16 * 16 * 16, dtype="f4").reshape(16, 16, 16))

def w_compound(path):
    import h5py, numpy as np
    dt = np.dtype([("id", "i4"), ("x", "f8"), ("y", "f8")])
    arr = np.zeros(256, dtype=dt)
    arr["id"] = np.arange(256); arr["x"] = np.arange(256) * 1.5; arr["y"] = -np.arange(256)
    with h5py.File(path, "w") as f:
        f.create_dataset("t", data=arr)

def w_strings(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("fixed", data=np.array([b"alpha", b"beta", b"gamma"], dtype="S8"))
        f.create_dataset("vlen", data=["one", "two", "three"],
                         dtype=h5py.string_dtype())

def w_attrs(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        d = f.create_dataset("d", data=np.arange(10, dtype="i4"))
        d.attrs["units"] = "meters"
        d.attrs["scale"] = np.float64(2.5)
        d.attrs["shape3"] = np.arange(3, dtype="i8")
        g = f.create_group("grp")
        g.attrs["title"] = "a group"
        f.attrs["root_note"] = "file-level attr"

def w_groups_links(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        g = f.create_group("outer/inner")
        ds = g.create_dataset("leaf", data=np.arange(20, dtype="i4"))
        f["hardlink"] = ds                      # hard link to the dataset
        f["softlink"] = h5py.SoftLink("/outer/inner/leaf")

def w_chunked_shuffle(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("s", data=np.arange(4096, dtype="i4").reshape(64, 64),
                         chunks=(16, 16), shuffle=True)

def w_hyperslab_src(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("g", data=np.arange(100 * 100, dtype="f4").reshape(100, 100),
                         chunks=(25, 100))

def w_uint_types(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("u8", data=np.arange(256, dtype="u1"))
        f.create_dataset("u16", data=np.arange(4096, dtype="u2"))
        f.create_dataset("u32", data=(np.arange(1024, dtype="u4") * 100003))

def w_enum(path):
    import h5py, numpy as np
    dt = h5py.enum_dtype({"RED": 0, "GREEN": 1, "BLUE": 2}, basetype="i4")
    with h5py.File(path, "w") as f:
        f.create_dataset("e", data=np.array([0, 1, 2, 1, 0, 2, 2], dtype="i4"), dtype=dt)

def w_array_dtype(path):
    import h5py, numpy as np
    dt = np.dtype(("f8", (3,)))
    with h5py.File(path, "w") as f:
        f.create_dataset("arr", data=np.arange(50 * 3, dtype="f8").reshape(50, 3), dtype=dt)

def w_scalar(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("sc", data=np.float64(3.14159265358979))

def w_extendible_append(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        d = f.create_dataset("ext", shape=(4,), maxshape=(None,), chunks=(4,), dtype="i4")
        d[:] = np.arange(4, dtype="i4")
    with h5py.File(path, "r+") as f:            # reopen and grow
        d = f["ext"]
        d.resize((8,))
        d[4:8] = np.arange(4, 8, dtype="i4")

def w_fletcher32(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("fl", data=np.arange(4096, dtype="i4").reshape(64, 64),
                         chunks=(16, 16), fletcher32=True)

def w_point_src(path):
    import h5py, numpy as np
    with h5py.File(path, "w") as f:
        f.create_dataset("p", data=np.arange(1000, dtype="f8"))

def read_point(path):
    import h5py
    h = hashlib.sha256()
    with h5py.File(path, "r") as f:
        sub = f["p"][[0, 10, 33, 100, 777, 999]]   # scattered point/fancy selection
        h.update(("%s|%s" % (sub.dtype, sub.shape)).encode())
        h.update(sub.tobytes())
    return h.hexdigest()

# ---------------------------------------------------------------- readers/digests
def digest_by_spec(path, paths, attrs):
    """sha256 over declared dataset paths (dtype/shape/bytes) + declared attrs.
    Accesses objects BY NAME only — no visititems/H5Ovisit (the VOL does not
    support link iteration; that is tested separately by the 'iteration' case)."""
    import h5py
    h = hashlib.sha256()
    with h5py.File(path, "r") as f:
        for p in paths:
            o = f[p]
            d = o[()]
            h.update(("D|%s|%s|%s" % (p, o.dtype, o.shape)).encode())
            h.update(d.tobytes() if hasattr(d, "tobytes") else repr(d).encode())
        for (op, an) in attrs:
            obj = f if op == "/" else f[op]
            v = obj.attrs[an]
            val = v.tolist() if hasattr(v, "tolist") else v
            h.update(("A|%s|%s|%r" % (op, an, val)).encode())
    return h.hexdigest()


def read_iteration(path):
    """Structure iteration through whatever VOL is active (visititems). Exercises
    H5Ovisit/H5Literate — a known VOL gap; expected to differ/fail through the VOL."""
    import h5py
    h = hashlib.sha256()
    names = []
    with h5py.File(path, "r") as f:
        f.visititems(lambda n, o: names.append(n))
    h.update("|".join(sorted(names)).encode())
    return h.hexdigest()

def read_hyperslab(path):
    """Selection read: a strided hyperslab through whatever VOL is active."""
    import h5py
    h = hashlib.sha256()
    with h5py.File(path, "r") as f:
        sub = f["g"][10:90:3, 5:95:2]        # strided 2D hyperslab
        h.update(("%s|%s" % (sub.dtype, sub.shape)).encode())
        h.update(sub.tobytes())
    return h.hexdigest()

CASES = {
    "int32_1d_contig":   {"write": w_int32_1d_contig, "paths": ["a"]},
    "float64_2d_chunked": {"write": w_float64_2d_chunked, "paths": ["m"]},
    "float32_3d_contig": {"write": w_float32_3d_contig, "paths": ["v"]},
    "compound":          {"write": w_compound, "paths": ["t"]},
    "strings":           {"write": w_strings, "paths": ["fixed", "vlen"]},
    "attrs":             {"write": w_attrs, "paths": ["d"],
                          "attrs": [("d", "units"), ("d", "scale"), ("d", "shape3"),
                                    ("grp", "title"), ("/", "root_note")]},
    "groups_links":      {"write": w_groups_links,
                          "paths": ["outer/inner/leaf", "hardlink", "softlink"]},
    "chunked_shuffle":   {"write": w_chunked_shuffle, "paths": ["s"]},
    "hyperslab_read":    {"write": w_hyperslab_src, "read": read_hyperslab},
    "uint_types":        {"write": w_uint_types, "paths": ["u8", "u16", "u32"]},
    "enum":              {"write": w_enum, "paths": ["e"]},
    "array_dtype":       {"write": w_array_dtype, "paths": ["arr"]},
    "scalar":            {"write": w_scalar, "paths": ["sc"]},
    "extendible_append": {"write": w_extendible_append, "paths": ["ext"]},
    "fletcher32":        {"write": w_fletcher32, "paths": ["fl"]},
    "point_selection":   {"write": w_point_src, "read": read_point},
    "iteration":         {"write": w_groups_links, "read": read_iteration},
}

# ---------------------------------------------------------------- worker
def worker(case, action, path):
    spec = CASES[case]
    if action == "write":
        spec["write"](path)
        print("WROTE")
    else:  # read
        rd = spec.get("read")
        if rd is None:
            digest = digest_by_spec(path, spec["paths"], spec.get("attrs", []))
        else:
            digest = rd(path)
        print("DIGEST:" + digest)

# ---------------------------------------------------------------- driver
def _env(vol):
    e = dict(os.environ, HOME="/home/iowarp",
             LD_LIBRARY_PATH=BIN + ":/usr/local/lib:/usr/lib/x86_64-linux-gnu",
             PYTHONPATH=BIN)
    if vol:
        e["HDF5_PLUGIN_PATH"] = BIN
        e["HDF5_VOL_CONNECTOR"] = "iowarp"
    else:
        e.pop("HDF5_VOL_CONNECTOR", None)
    return e

def _run(case, action, path, vol):
    cmd = [sys.executable, os.path.abspath(__file__), "--worker",
           "--case", case, "--action", action, "--file", path]
    p = subprocess.run(cmd, capture_output=True, text=True, env=_env(vol), timeout=180)
    for line in p.stdout.splitlines():
        if line.startswith("DIGEST:"):
            return line[7:], p.returncode
        if line == "WROTE":
            return "WROTE", p.returncode
    return None, p.returncode  # crash / no output

def _h5diff(a, b):
    p = subprocess.run(["h5diff", a, b], capture_output=True, text=True,
                       env=_env(False))
    return p.returncode == 0

def _tool_ok(path):
    for tool in (["h5dump", "-H"], ["h5ls", "-r"]):
        p = subprocess.run(tool + [path], capture_output=True, text=True, env=_env(False))
        if p.returncode != 0:
            return False
    return True

def restart_runtime():
    """Kill any clio_run, wipe shm, start fresh from BIN, wait until ready."""
    subprocess.run(["pkill", "-f", "clio_run"], check=False)
    time.sleep(2)
    for f in os.listdir("/dev/shm"):
        if f.startswith("chimaera") or f.startswith("clio"):
            try:
                os.remove(os.path.join("/dev/shm", f))
            except OSError:
                pass
    env = dict(os.environ, HOME=CLIO_HOME)
    with open(RUNTIME_LOG, "w") as log:
        subprocess.Popen([os.path.join(BIN, "clio_run"), "start"], stdout=log,
                         stderr=log, cwd=os.path.dirname(BIN) or "/", env=env)
    for _ in range(60):
        try:
            with open(RUNTIME_LOG) as fh:
                if RUNTIME_READY in fh.read():
                    time.sleep(1)
                    return True
        except FileNotFoundError:
            pass
        time.sleep(1)
    return False


def driver(args):
    assert restart_runtime(), "clio_run did not become ready"
    os.makedirs(TMP, exist_ok=True)
    results, n_fail = {}, 0
    for case in CASES:
        fn = os.path.join(TMP, case + "_native.h5")
        fv = os.path.join(TMP, case + "_vol.h5")
        for f in (fn, fv):
            if os.path.exists(f):
                os.remove(f)
        # arm setup
        _run(case, "write", fn, vol=False)
        ref, _ = _run(case, "read", fn, vol=False)          # reference (native/native)
        _run(case, "write", fv, vol=True)
        d_wc, rc_wc = _run(case, "read", fv, vol=False)      # write compat
        d_rc, rc_rc = _run(case, "read", fn, vol=True)       # read compat
        d_rt, rc_rt = _run(case, "read", fv, vol=True)       # round-trip
        props = {
            "write_compat": (ref is not None and d_wc == ref),
            "read_compat":  (ref is not None and d_rc == ref),
            "roundtrip":    (ref is not None and d_rt == ref),
            "h5diff_clean": (os.path.exists(fn) and os.path.exists(fv) and _h5diff(fn, fv)),
            "tools_ok":     (os.path.exists(fv) and _tool_ok(fv)),
        }
        results[case] = props
        if not all(props.values()):
            n_fail += 1
        mark = lambda b: "PASS" if b else "FAIL"
        print(f"  {case:<20} " + " ".join(f"{k}={mark(v)}" for k, v in props.items()))
    with open(args.out, "w") as f:
        json.dump(results, f, indent=2)
    total = len(CASES)
    expect_fail = {x for x in (args.expect_fail or "").split(",") if x}
    failed = {c for c, p in results.items() if not all(p.values())}
    regressions = sorted(failed - expect_fail)     # green case broke -> CI red
    fixed = sorted(expect_fail - failed)           # known gap now passes -> shrink baseline
    print(f"\n{total - len(failed)}/{total} cases fully pass. wrote {args.out}")
    if expect_fail:
        print(f"expected gaps still failing: {sorted(failed & expect_fail)}")
    if fixed:
        print(f"NOTE: previously-failing cases now PASS (remove from --expect-fail): {fixed}")
    if regressions:
        print(f"REGRESSION — these should pass but FAILED: {regressions}")
    return 1 if regressions else 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worker", action="store_true")
    ap.add_argument("--case")
    ap.add_argument("--action", choices=["write", "read"])
    ap.add_argument("--file")
    ap.add_argument("--out", default="vol_compat_results.json")
    ap.add_argument("--bin", help="dir with libiowarp_hdf5_vol.so + clio_run "
                    "(default $CLIO_VOL_BIN or /workspace/build/bin)")
    ap.add_argument("--expect-fail", default="",
                    help="comma-separated cases allowed to fail (known gaps); "
                    "exit is nonzero only on a REGRESSION outside this set")
    a = ap.parse_args()
    if a.bin:
        global BIN
        BIN = a.bin
    if a.worker:
        worker(a.case, a.action, a.file)
        return 0
    return driver(a)

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env bash
#
# Drive the SCRATCH-requiring xfstests against the clio filesystem.
#
# Unlike run_clio_xfstests.sh (which leaves SCRATCH_DEV unset, so every
# scratch-requiring test 'notrun's), this driver gives xfstests a real, ISOLATED
# scratch device using xfstests' native fuse support -- no clio adapter changes:
#
#   * Two persistent clio runtimes on well-separated ports (a runtime binds
#     port, port+1 and port+3, so they must be >=4 apart): one backs TEST_DIR,
#     the other backs SCRATCH_MNT. Two runtimes == two fully isolated
#     namespaces, so `_scratch_mkfs`'s `rm -rf $SCRATCH_MNT/*` can't touch TEST.
#     The runtimes persist across the mount/unmount cycles ./check performs
#     between (and within) tests.
#   * A /sbin/mount.fuse.<subtyp> helper + FUSE_SUBTYP in local.config, so
#     `./check` mounts and unmounts TEST_DIR/SCRATCH_MNT itself (via
#     `mount -t fuse.<subtyp>`), exactly as xfstests expects for a fuse fs
#     (see xfstests' README.fuse). The helper execs a daemonizing clio_cte_fuse
#     client, choosing the runtime port from the device string.
#
# Good-citizen isolation (other agents/CI jobs may run clio concurrently):
#   * RENAMED binary copies, so the stock driver's `pkill -9 -x clio_run`/
#     `clio_cte_fuse` (run per-test in run_clio_xfstests.sh) can't kill ours.
#   * A PRIVATE xfstests copy, so we never clobber a shared local.config/results.
#   * We only ever kill our own uniquely-named processes -- never a broad pkill.
#
# Prereqs: same as run_clio_xfstests.sh, plus passwordless sudo (to install the
# mount helper into /sbin) and `unshare` (./check needs euid 0).
#
# Usage:
#   scripts/xfstests/run_clio_scratch_xfstests.sh [test-list...]
#   TESTS="generic/013 generic/029" scripts/xfstests/run_clio_scratch_xfstests.sh
#
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_BIN="${CLIO_BUILD_DIR:-${REPO_ROOT}/build}/bin"

# --- locate xfstests --------------------------------------------------------
if [ -z "${XFSTESTS_DIR:-}" ]; then
  for cand in /opt/xfstests "${REPO_ROOT}/external/xfstests"; do
    [ -x "${cand}/check" ] && XFSTESTS_DIR="${cand}" && break
  done
fi
if [ -z "${XFSTESTS_DIR:-}" ] || [ ! -x "${XFSTESTS_DIR}/check" ]; then
  echo "ERROR: xfstests not found/built. Set XFSTESTS_DIR." >&2
  exit 1
fi
[ -x "${BUILD_BIN}/clio_cte_fuse" ] || { echo "ERROR: clio_cte_fuse not in ${BUILD_BIN}" >&2; exit 1; }
[ -x "${BUILD_BIN}/clio_run" ]      || { echo "ERROR: clio_run not in ${BUILD_BIN}" >&2; exit 1; }
command -v fusermount3 >/dev/null || { echo "ERROR: fusermount3 (fuse3) missing" >&2; exit 1; }
command -v unshare >/dev/null     || { echo "ERROR: unshare (util-linux) missing" >&2; exit 1; }

# --- config (all overridable) -----------------------------------------------
PRIV="${CLIO_SCRATCH_PRIV:-/tmp/clio_scratch_xfs}"
TEST_PORT="${CLIO_SCRATCH_TEST_PORT:-7000}"
SCRATCH_PORT="${CLIO_SCRATCH_SCRATCH_PORT:-7010}"   # >=4 above TEST_PORT
SUBTYP="${CLIO_SCRATCH_SUBTYP:-cliofs}"             # /sbin/mount.fuse.<SUBTYP>
PERTEST="${CLIO_XFS_PERTEST_TIMEOUT:-90}"
HELPER="/sbin/mount.fuse.${SUBTYP}"
CLR="${PRIV}/clr_${SUBTYP}"        # renamed clio_run     (pkill-immune)
CLF="${PRIV}/clf_${SUBTYP}"        # renamed clio_cte_fuse
TEST_DIR="${PRIV}/test"
SCRATCH_MNT="${PRIV}/scratch"
PRIV_XFS="${PRIV}/xfstests"        # private xfstests copy (isolated config/results)
RESULTS="${PRIV}/results.txt"

export LD_LIBRARY_PATH="${BUILD_BIN}:${HOME}/.local/lib:${LD_LIBRARY_PATH:-}"
export CLIO_REPO_PATH="${BUILD_BIN}" CLIO_BIND_ADDR=127.0.0.1

# --- root re-exec: install the mount helper (needs real root) BEFORE the
#     user namespace, where /sbin is read-only; then re-exec under unshare -rm.
if [ "$(id -u)" -ne 0 ] && [ -z "${CLIO_SCRATCH_INNS:-}" ]; then
  echo "[scratch-xfs] installing ${HELPER}"
  sudo tee "${HELPER}" >/dev/null <<EOF
#!/bin/bash
# xfstests invokes: ${HELPER} <device> <mountpoint> [-o opts]
# Pick the runtime port from the device string; exec a daemonizing client.
dev="\$1"; mnt="\$2"; shift 2 2>/dev/null
case "\$dev" in *scratch*) port=${SCRATCH_PORT} ;; *) port=${TEST_PORT} ;; esac
export LD_LIBRARY_PATH="${BUILD_BIN}:${HOME}/.local/lib" CLIO_REPO_PATH="${BUILD_BIN}"
export CLIO_BIND_ADDR=127.0.0.1 CLIO_PORT=\$port CLIO_WITH_RUNTIME=0
exec "${CLF}" "\$mnt" -o "fsname=\$dev,allow_other,default_permissions"
EOF
  sudo chmod +x "${HELPER}"
  exec env CLIO_SCRATCH_INNS=1 unshare -rm "$0" "$@"
fi

# --- inside namespaced root -------------------------------------------------
mkdir -p "${PRIV}" "${TEST_DIR}" "${SCRATCH_MNT}"
cp -f "${BUILD_BIN}/clio_run" "${CLR}"
cp -f "${BUILD_BIN}/clio_cte_fuse" "${CLF}"

# Private xfstests copy so we never clobber a shared local.config / results dir.
if [ ! -x "${PRIV_XFS}/check" ]; then
  echo "[scratch-xfs] creating private xfstests copy at ${PRIV_XFS}"
  rm -rf "${PRIV_XFS}"; cp -a "${XFSTESTS_DIR}" "${PRIV_XFS}"
fi
rm -f "${PRIV_XFS}/local.config"; rm -rf "${PRIV_XFS}/results"; mkdir -p "${PRIV_XFS}/results"
: > "${RESULTS}"

RTT=""; RTS=""
cleanup() {
  fusermount3 -u "${TEST_DIR}" 2>/dev/null; fusermount3 -u "${SCRATCH_MNT}" 2>/dev/null
  [ -n "${RTT}" ] && kill -9 "${RTT}" 2>/dev/null
  [ -n "${RTS}" ] && kill -9 "${RTS}" 2>/dev/null
  # Only our uniquely-named procs -- never a broad clio_run/clio_cte_fuse pkill.
  pkill -9 -x "clr_${SUBTYP}" 2>/dev/null; pkill -9 -x "clf_${SUBTYP}" 2>/dev/null
}
trap cleanup EXIT

start_runtime() {  # $1=port  $2=logfile -> echoes pid
  env CLIO_PORT="$1" CLIO_WITH_RUNTIME=1 "${CLR}" start >"$2" 2>&1 &
  echo $!
}
echo "[scratch-xfs] starting runtimes: TEST=${TEST_PORT} SCRATCH=${SCRATCH_PORT}"
RTT="$(start_runtime "${TEST_PORT}"    "${PRIV}/rt_test.log")"
RTS="$(start_runtime "${SCRATCH_PORT}" "${PRIV}/rt_scratch.log")"
sleep 8
kill -0 "${RTT}" 2>/dev/null || { echo "ERROR: TEST runtime failed"    >&2; tail -5 "${PRIV}/rt_test.log"    >&2; exit 1; }
kill -0 "${RTS}" 2>/dev/null || { echo "ERROR: SCRATCH runtime failed" >&2; tail -5 "${PRIV}/rt_scratch.log" >&2; exit 1; }

# --- xfstests native fuse config --------------------------------------------
cat > "${PRIV_XFS}/local.config" <<EOF
export FSTYP=fuse
export FUSE_SUBTYP=.${SUBTYP}
export TEST_DEV=clio_test
export TEST_DIR=${TEST_DIR}
export SCRATCH_DEV=clio_scratch
export SCRATCH_MNT=${SCRATCH_MNT}
EOF

# --- resolve test list (expand groups via ./check -n) -----------------------
cd "${PRIV_XFS}" || exit 1
if [ "$#" -gt 0 ]; then RAW=("$@")
elif [ -n "${TESTS:-}" ]; then RAW=(${TESTS})  # shellcheck disable=SC2206
else RAW=(generic/013 generic/029 generic/030); fi
mapfile -t LIST < <(./check -n "${RAW[@]}" 2>/dev/null | grep -E '^[a-z_]+/[0-9]+')
[ "${#LIST[@]}" -eq 0 ] && LIST=("${RAW[@]}")
echo "[scratch-xfs] running ${#LIST[@]} scratch test(s)"

# --- run, one test at a time, with per-test timeout + runtime recovery ------
pass=0; fail=0; notrun=0; hang=0; failed_list=""
for t in "${LIST[@]}"; do
  kill -0 "${RTT}" 2>/dev/null || { RTT="$(start_runtime "${TEST_PORT}"    "${PRIV}/rt_test.log")";    sleep 5; }
  kill -0 "${RTS}" 2>/dev/null || { RTS="$(start_runtime "${SCRATCH_PORT}" "${PRIV}/rt_scratch.log")"; sleep 5; }
  out="$(timeout "${PERTEST}" ./check "${t}" 2>/dev/null)"; rc=$?
  # Order matters: a notrun test still prints "Passed all 0/1 tests", so the
  # "Not run:" check MUST precede the "Passed all" check.
  if   [ "${rc}" -eq 124 ];                   then st=HANG;   hang=$((hang+1));   failed_list+=" ${t}(hang)"
  elif echo "${out}" | grep -q "^Not run:";   then st=notrun; notrun=$((notrun+1))
  elif echo "${out}" | grep -q "^Passed all"; then st=pass;   pass=$((pass+1))
  else                                             st=FAIL;   fail=$((fail+1));   failed_list+=" ${t}"; fi
  echo "${t}: ${st}"; echo "${t}: ${st}" >>"${RESULTS}"
  fusermount3 -u "${TEST_DIR}" 2>/dev/null; fusermount3 -u "${SCRATCH_MNT}" 2>/dev/null
done

echo "===================================================================="
echo "[scratch-xfs] TALLY: pass=${pass} fail=${fail} notrun=${notrun} hang=${hang} (list=${#LIST[@]})"
[ -n "${failed_list}" ] && echo "[scratch-xfs] not passing:${failed_list}"
exit 0

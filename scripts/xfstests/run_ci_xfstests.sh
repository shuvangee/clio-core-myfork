#!/usr/bin/env bash
#
# CI regression gate for the clio FUSE filesystem xfstests conformance suite.
#
# Runs the curated generic/* list (the union of the expected-pass baseline and
# the known-failure list) via run_clio_xfstests.sh, then compares the outcome
# against scripts/xfstests/ci_baseline_pass.txt:
#
#   * A baseline test that regresses (no longer 'pass')      -> FAIL the job.
#   * A test missing from the run output (didn't execute)    -> FAIL the job.
#   * A known-failure that now passes                        -> report, don't
#                                                               fail; promote it
#                                                               into the baseline.
#
# This keeps CI green at the recorded baseline while surfacing both regressions
# and improvements. See issue #677.
#
# Usage:
#   scripts/xfstests/run_ci_xfstests.sh
# Honors the same env as run_clio_xfstests.sh (CLIO_BUILD_DIR, XFSTESTS_DIR,
# CLIO_XFS_PERTEST_TIMEOUT, ...).
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BASELINE_FILE="${SCRIPT_DIR}/ci_baseline_pass.txt"
KNOWN_FAIL_FILE="${SCRIPT_DIR}/ci_known_fail.txt"
RUNNER="${SCRIPT_DIR}/run_clio_xfstests.sh"

[ -r "${BASELINE_FILE}" ]   || { echo "ERROR: missing ${BASELINE_FILE}" >&2; exit 2; }
[ -x "${RUNNER}" ] || [ -r "${RUNNER}" ] || { echo "ERROR: missing ${RUNNER}" >&2; exit 2; }

# --- read test lists (strip comments / blanks) ------------------------------
read_list() { grep -oE '^generic/[0-9]+' "$1" 2>/dev/null | sort -u; }
mapfile -t BASELINE   < <(read_list "${BASELINE_FILE}")
mapfile -t KNOWN_FAIL < <(read_list "${KNOWN_FAIL_FILE}")

[ "${#BASELINE[@]}" -gt 0 ] || { echo "ERROR: baseline is empty" >&2; exit 2; }

# Union = everything we want to run this cycle.
mapfile -t ALL < <(printf '%s\n' "${BASELINE[@]}" "${KNOWN_FAIL[@]}" | sort -u)

echo "[ci-xfs] baseline(pass)=${#BASELINE[@]} known-fail=${#KNOWN_FAIL[@]} total=${#ALL[@]}"

# --- run the conformance driver, capture per-test outcomes ------------------
RUN_LOG="$(mktemp)"
trap 'rm -f "${RUN_LOG}"' EXIT
TESTS="${ALL[*]}" bash "${RUNNER}" 2>&1 | tee "${RUN_LOG}"

# Outcome lines look like: 'generic/001 : pass' / 'generic/013 : FAIL' / notrun / HANG
status_of() {  # $1 = test id -> prints pass|FAIL|notrun|HANG|MISSING
  local s
  s="$(grep -oE "^$1 : [a-zA-Z]+" "${RUN_LOG}" | tail -1 | awk '{print $NF}')"
  [ -n "${s}" ] && echo "${s}" || echo "MISSING"
}

# --- evaluate baseline (the regression gate) --------------------------------
regressions=()
for t in "${BASELINE[@]}"; do
  st="$(status_of "${t}")"
  [ "${st}" = "pass" ] || regressions+=("${t}(${st})")
done

# --- evaluate known-failures (informational: did any start passing?) --------
unexpected_pass=()
for t in "${KNOWN_FAIL[@]}"; do
  st="$(status_of "${t}")"
  [ "${st}" = "pass" ] && unexpected_pass+=("${t}")
done

echo "===================================================================="
echo "[ci-xfs] SUMMARY"
echo "[ci-xfs]   baseline size       : ${#BASELINE[@]}"
echo "[ci-xfs]   regressions         : ${#regressions[@]}"
echo "[ci-xfs]   newly passing (info): ${#unexpected_pass[@]}"

if [ "${#unexpected_pass[@]}" -gt 0 ]; then
  echo "[ci-xfs] NOTE: known-failures now passing -- promote into ci_baseline_pass.txt:"
  printf '[ci-xfs]   + %s\n' "${unexpected_pass[@]}"
fi

if [ "${#regressions[@]}" -gt 0 ]; then
  echo "[ci-xfs] FAIL: ${#regressions[@]} baseline test(s) regressed:"
  printf '[ci-xfs]   - %s\n' "${regressions[@]}"
  exit 1
fi

echo "[ci-xfs] OK: all ${#BASELINE[@]} baseline tests still pass."
exit 0

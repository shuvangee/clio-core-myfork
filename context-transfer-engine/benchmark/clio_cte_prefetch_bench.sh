#!/usr/bin/env bash
# Launch the prefetch benchmark against an embedded, live two-tier CLIO Core.
# Override CLIO_SERVER_CONF or CLIO_PREFETCH_BENCH_EXE when needed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export CLIO_SERVER_CONF="${CLIO_SERVER_CONF:-${SCRIPT_DIR}/cte_prefetch_bench_config.yaml}"
export CLIO_BENCH_SELF_RUN=1
BENCH_EXE="${CLIO_PREFETCH_BENCH_EXE:-clio_cte_prefetch_bench}"

if [[ ! -f "${CLIO_SERVER_CONF}" ]]; then
  echo "Two-tier configuration not found: ${CLIO_SERVER_CONF}" >&2
  exit 1
fi
if ! command -v "${BENCH_EXE}" >/dev/null 2>&1 && [[ ! -x "${BENCH_EXE}" ]]; then
  echo "Benchmark executable not found: ${BENCH_EXE}" >&2
  echo "Set CLIO_PREFETCH_BENCH_EXE to the built executable path." >&2
  exit 1
fi

exec "${BENCH_EXE}" "$@"

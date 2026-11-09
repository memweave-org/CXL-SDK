#!/usr/bin/env bash
# Run on both hosts: UB BwTree NO OPT and OPT, using config.sh.
# Supports run_all.sh options, including --dry-run, --thread-counts,
# --build-only, --bundles and --output. The backend is always UB.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
set -a
source "${YCSB_UB_CONFIG:-./config.sh}"
export YCSB_BACKEND=ub
set +a
exec python3 - "$@" --backend ub <<'PY'
import run_all

run_all.CASES = ("bwtree", "bwtree+opt")
raise SystemExit(run_all.main())
PY

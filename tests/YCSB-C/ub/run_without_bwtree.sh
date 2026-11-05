#!/usr/bin/env bash
# Run on both hosts. Skip bwtree and bwtree+opt; keep bwtree+rpc.
# Accepts the same arguments as run_all.sh, including --dry-run and --bundles.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
set -a
source "${YCSB_UB_CONFIG:-./config.sh}"
set +a
exec python3 - "$@" <<'PY'
import run_all

run_all.CASES = tuple(
    case for case in run_all.CASES
    if case not in ("bwtree", "bwtree+opt")
)
raise SystemExit(run_all.main())
PY

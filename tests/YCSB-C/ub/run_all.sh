#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
set -a
source "${YCSB_UB_CONFIG:-./config.sh}"
set +a
exec python3 ./run_all.py "$@"

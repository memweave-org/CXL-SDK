#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
set -a
source "${YCSB_UB_CONFIG:-./config.sh}"
THREAD_COUNTS=${THREAD_COUNTS:-"32 64 128"}
set +a
exec python3 ./run_threads.py "$@"

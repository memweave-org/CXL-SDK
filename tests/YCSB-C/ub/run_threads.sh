#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
set -a
source "${YCSB_UB_CONFIG:-./config.sh}"
if [[ ${YCSB_BACKEND:-ub} == cxl ]]; then
  if [[ ${YCSB_UB_CASE:-bwtree} == bwtree+rpc ]]; then
    THREAD_COUNTS=${THREAD_COUNTS:-24}
  else
    THREAD_COUNTS=${THREAD_COUNTS:-32}
  fi
else
  THREAD_COUNTS=${THREAD_COUNTS:-"32 64 128"}
fi
set +a
exec python3 ./run_threads.py "$@"

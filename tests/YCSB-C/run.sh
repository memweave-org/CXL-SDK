#!/usr/bin/env bash
set -euo pipefail
if [[ ${YCSB_UB_CASE:-bwtree} == bwtree && ${YCSB_UB_SUITE:-0} == 0 && ${YCSB_UB_SINGLE:-0} != 1 ]]; then
  exec bash "$(dirname -- "${BASH_SOURCE[0]}")/ub/run_threads.sh" "$@"
fi
exec bash "$(dirname -- "${BASH_SOURCE[0]}")/ub/run.sh" "$@"

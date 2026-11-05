#!/usr/bin/env bash
set -euo pipefail
args=()
while (($#)); do
  case $1 in
    --backend) export YCSB_BACKEND=${2:?--backend requires ub or cxl}; shift 2;;
    --backend=*) export YCSB_BACKEND=${1#*=}; shift;;
    *) args+=("$1"); shift;;
  esac
done
set -- "${args[@]}"
export YCSB_BACKEND=${YCSB_BACKEND:-ub}
case $YCSB_BACKEND in
  ub|cxl) ;;
  *) echo 'Backend must be ub or cxl' >&2; exit 2;;
esac
if [[ $YCSB_BACKEND == cxl ]] || [[ ${YCSB_UB_CASE:-bwtree} == bwtree && ${YCSB_UB_SUITE:-0} == 0 && ${YCSB_UB_SINGLE:-0} != 1 ]]; then
  exec bash "$(dirname -- "${BASH_SOURCE[0]}")/ub/run_threads.sh" "$@"
fi
exec bash "$(dirname -- "${BASH_SOURCE[0]}")/ub/run.sh" "$@"

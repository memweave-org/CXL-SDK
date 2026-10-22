#!/usr/bin/env bash
# Reuse only the Docker/SDK setup; no YCSB builds or worker processes are started.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
mode=${1:-init}
if [[ $mode == --help || $mode == -h ]]; then
  echo 'Usage: run_sdk_smoke.sh init | cache|noncache PROVIDER [SIZE_MB]'
  echo 'Supports UB_IMAGE / UB_CONTAINER and SDK variables from run_ub_single.sh.'
  echo 'Default size: 128 MiB. Cache mode requires the API-documented BIOS/ownership setup.'
  exit 0
fi
case $mode in init|cache|noncache) ;; *) echo 'Mode must be init, cache or noncache' >&2; exit 2;; esac
if [[ $mode != init && -z ${2:-${UBSM_PROVIDER_HOST:-}} ]]; then
  echo 'Specify the memory provider UB topology hostname' >&2; exit 2
fi
if (( $# > 3 )); then echo 'Too many arguments' >&2; exit 2; fi
export UB_SDK_TEST_MODE=$mode
export SIZE_MB=${3:-${SIZE_MB:-128}}
exec bash "$ROOT/tests/two-process-ycsb/run_ub_single.sh" "${2:-${UBSM_PROVIDER_HOST:-unused-for-init}}"

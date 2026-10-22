#!/usr/bin/env bash
# Compile and smoke-test two OPT indexes using the real UBSM backend.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ ${1:-} == -h || ${1:-} == --help ]]; then
  cat <<'EOF'
Usage: ./tests/two-process-ycsb/run_ub_single.sh [PROVIDER_HOST [THREADS0 [THREADS1]]]

Build and run BwTree OPT and ClevelHash OPT, each with two local processes.
PROVIDER_HOST must be the local machine's name in the UB topology; alternatively
set UBSM_PROVIDER_HOST. Default threads: 2 per process.

Optional environment:
  UB_CONTAINER        Existing UB-enabled Docker container; build/run inside it.
                      The repository must be mounted at the same absolute path.
  UBSM_INCLUDE_DIR     Default /usr/local/ubs_mem/include
  UBSM_LIBRARY         Default /usr/local/ubs_mem/lib/libubsm_sdk.so
  UBSM_PROVIDER_NUMA  Default UINT32_MAX (SDK chooses); set to actual memory NUMA.
  RECORDS=10000 OPS=10000 SIZE_MB=1024 JOBS=8 TIMEOUT=300
  OUTPUT              New results directory (default unique directory in build/).

Requires vendor SDK, running UB services and device/socket/group access.
This script never switches to file memory or the mock SDK automatically.
EOF
  exit 0
fi
if (( $# > 3 )); then
  echo 'Too many arguments; use --help' >&2; exit 2
fi

# Re-enter the same script inside an already configured UB container. Preserve
# its SDK/runtime environment unless the caller explicitly overrides it.
if [[ -n ${UB_CONTAINER:-} ]]; then
  env_args=(-e UB_CONTAINER=)
  for name in UBSM_PROVIDER_HOST UBSM_PROVIDER_NUMA UBSM_INCLUDE_DIR UBSM_LIBRARY \
      LD_LIBRARY_PATH HCOM_CONNECTION_RETRY_TIMES RECORDS OPS SIZE_MB JOBS TIMEOUT OUTPUT; do
    if [[ -v $name ]]; then env_args+=(-e "$name=${!name}"); fi
  done
  exec docker exec "${env_args[@]}" -w "$ROOT" "$UB_CONTAINER" \
    bash "$ROOT/tests/two-process-ycsb/run_ub_single.sh" "$@"
fi

provider=${1:-${UBSM_PROVIDER_HOST:-}}
if [[ -z $provider ]]; then
  echo 'Specify the local UB topology hostname as argument 1 or UBSM_PROVIDER_HOST; see --help.' >&2
  exit 2
fi
export UBSM_INCLUDE_DIR=${UBSM_INCLUDE_DIR:-/usr/local/ubs_mem/include}
export UBSM_LIBRARY=${UBSM_LIBRARY:-/usr/local/ubs_mem/lib/libubsm_sdk.so}
for file in "$UBSM_INCLUDE_DIR/ubs_mem.h" "$UBSM_INCLUDE_DIR/ubs_mem_def.h" "$UBSM_LIBRARY"; do
  if [[ ! -r $file ]]; then echo "Missing UBSM SDK file: $file" >&2; exit 2; fi
done
export LD_LIBRARY_PATH="$(dirname -- "$UBSM_LIBRARY")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HCOM_CONNECTION_RETRY_TIMES=${HCOM_CONNECTION_RETRY_TIMES:-2}
cd "$ROOT"
output=${OUTPUT:-build/ub-single-$(date +%Y%m%d-%H%M%S)-$$}
if [[ $output != /* ]]; then output="$ROOT/$output"; fi
echo "UBSM single-host: provider=$provider, threads=${2:-2}/${3:-${2:-2}}"
echo "Build logs, worker logs and results.json: $output"
trap 'rc=$?; if (( rc != 0 )); then echo "Failed; inspect logs in $output. If a UB object was created, confirm workers stopped before manual cleanup." >&2; fi' EXIT
python3 tests/two-process-ycsb/run.py \
  --cases bwtree-on clevelhash-on --memory ub --ub-api ubsm \
  --ub-library "$UBSM_LIBRARY" --ub-include "$UBSM_INCLUDE_DIR" \
  --provider-host "$provider" --provider-numa "${UBSM_PROVIDER_NUMA:-4294967295}" \
  --shared-path ub-single --threads "${2:-2}" "${3:-${2:-2}}" \
  --records "${RECORDS:-10000}" --ops "${OPS:-10000}" \
  --size-mb "${SIZE_MB:-1024}" --mapping-bits 16 \
  --jobs "${JOBS:-8}" --timeout "${TIMEOUT:-300}" --output "$output"

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
  UB_IMAGE            Docker image: automatically create/run/remove a container.
  UB_SDK_ROOT         Host SDK directory (default /usr/local/ubs_mem).
  UB_DOCKER_ARGS_FILE Optional Docker arguments, one argument per line (no eval).
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

if [[ -n ${UB_IMAGE:-} && -n ${UB_CONTAINER:-} ]]; then
  echo 'Use UB_IMAGE or UB_CONTAINER, not both.' >&2; exit 2
fi

# Re-enter the same script inside an already configured UB container. Preserve
# its SDK/runtime environment unless the caller explicitly overrides it.
if [[ -n ${UB_CONTAINER:-} || -n ${UB_IMAGE:-} ]]; then
  env_args=(-e UB_CONTAINER= -e UB_IMAGE=)
  for name in UBSM_PROVIDER_HOST UBSM_PROVIDER_NUMA UBSM_INCLUDE_DIR UBSM_LIBRARY \
      LD_LIBRARY_PATH HCOM_CONNECTION_RETRY_TIMES RECORDS OPS SIZE_MB JOBS TIMEOUT OUTPUT; do
    if [[ -v $name ]]; then env_args+=(-e "$name=${!name}"); fi
  done
  if [[ -n ${UB_IMAGE:-} ]]; then
    sdk_root=${UB_SDK_ROOT:-/usr/local/ubs_mem}
    if [[ ! -d $sdk_root || $sdk_root != /* ]]; then
      echo "UB_SDK_ROOT must be an existing absolute directory: $sdk_root" >&2; exit 2
    fi
    if [[ -z ${1:-${UBSM_PROVIDER_HOST:-}} ]]; then
      echo 'Specify the local UB topology hostname; see --help.' >&2; exit 2
    fi
    # OBMM creates /dev/obmm_shmdev* after allocation; a static --device list
    # cannot expose those future nodes. Keep host UID/groups and share /dev.
    run_args=(--rm --network host --uts host
      --user "$(id -u):$(id -g)" --device-cgroup-rule 'c *:* rw'
      -v "$ROOT:$ROOT" -w "$ROOT"
      -v /dev:/dev -v /run:/run:ro -v "$sdk_root:$sdk_root:ro")
    for gid in $(id -G); do run_args+=(--group-add "$gid"); done
    env_args+=(-e "UBSM_INCLUDE_DIR=${UBSM_INCLUDE_DIR:-$sdk_root/include}"
               -e "UBSM_LIBRARY=${UBSM_LIBRARY:-$sdk_root/lib/libubsm_sdk.so}")
    # Mount the known vendor dependencies individually, never replace /usr/lib64.
    for lib in /usr/lib64/libubse-client.so* /usr/lib64/libobmm.so*; do
      if [[ -f $lib ]]; then run_args+=(-v "$lib:$lib:ro"); fi
    done
    if [[ -n ${UB_DOCKER_ARGS_FILE:-} ]]; then
      [[ -r $UB_DOCKER_ARGS_FILE ]] || { echo "Cannot read $UB_DOCKER_ARGS_FILE" >&2; exit 2; }
      while IFS= read -r arg || [[ -n $arg ]]; do
        [[ -z $arg || $arg == \#* ]] || run_args+=("$arg")
      done < "$UB_DOCKER_ARGS_FILE"
    fi
    echo "Creating temporary UB container from image: $UB_IMAGE"
    exec docker run "${run_args[@]}" "${env_args[@]}" --entrypoint /bin/bash "$UB_IMAGE" \
      "$ROOT/tests/two-process-ycsb/run_ub_single.sh" "$@"
  fi
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

#!/usr/bin/env bash
# Compile and smoke-test two OPT indexes using the real UBSM backend.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ ${1:-} == -h || ${1:-} == --help ]]; then
  cat <<'EOF'
Usage: ./tests/two-process-ycsb/run_ub_single.sh [PROVIDER_HOST [THREADS0 [THREADS1]]]

Build and run BwTree OPT and ClevelHash OPT, each with two local processes.
PROVIDER_HOST is the memory provider's UB topology name (local OR remote); alternatively
set UBSM_PROVIDER_HOST. Both worker processes run here, not on the provider.
Data uses CACHE memory; atomics use a separate NONCACHE arena.
Default threads: 2 per process.

Optional environment:
  CASES               Space-separated cases (default: bwtree-on clevelhash-on).
                      Example: "sherman rpc bwtree-on clevelhash-on".
  UB_IMAGE            Docker build image; YCSB runs on the host after compilation.
                      Standalone UB_SDK_TEST_MODE tests still run in the container.
  UB_SDK_ROOT         Host SDK directory (default /usr/local/ubs_mem).
                      Auto-detect lib/lib64, including a sole versioned library.
  UB_EXTRA_LIBRARIES  Colon-separated SDK plugin paths (for dlopen dependencies).
  UB_EXTRA_LIBRARY_DIRS Additional colon-separated dependency search directories.
  UB_DOCKER_ARGS_FILE Optional Docker arguments, one argument per line (no eval).
  UB_CONTAINER        Existing UB-enabled Docker container; build/run inside it.
                      The repository must be mounted at the same absolute path.
  UBSM_INCLUDE_DIR     Default /usr/local/ubs_mem/include
  UBSM_LIBRARY         Default /usr/local/ubs_mem/lib/libubsm_sdk.so
  UBSM_PROVIDER_NUMA  Default UINT32_MAX (SDK chooses); set to actual memory NUMA.
  RECORDS=10000 OPS=10000 SIZE_MB=1024 ATOMIC_SIZE_MB=512 JOBS=8 TIMEOUT=300
  BwTree/ClevelHash use CACHE data and a separate NONCACHE atomic arena.
  OUTPUT              New results directory (default unique directory in build/).

Requires vendor SDK, running UB services and device/socket/group access.
This script never switches to file memory or the mock SDK automatically.
EOF
  exit 0
fi
if (( $# > 3 )); then
  echo 'Too many arguments; use --help' >&2; exit 2
fi

# Resolve on the host before docker run, or inside an existing container.
resolve_sdk() {
  sdk_root=${UB_SDK_ROOT:-/usr/local/ubs_mem}
  export UBSM_INCLUDE_DIR=${UBSM_INCLUDE_DIR:-$sdk_root/include}
  if [[ -z ${UBSM_LIBRARY:-} ]]; then
    for dir in "$sdk_root/lib" "$sdk_root/lib64"; do
      if [[ -r $dir/libubsm_sdk.so ]]; then UBSM_LIBRARY=$dir/libubsm_sdk.so; break; fi
    done
    if [[ -z ${UBSM_LIBRARY:-} ]]; then
      declare -A candidates=()
      for file in "$sdk_root"/lib/libubsm_sdk.so.* "$sdk_root"/lib64/libubsm_sdk.so.*; do
        [[ ! -f $file ]] || candidates["$(readlink -f -- "$file")"]=1
      done
      if (( ${#candidates[@]} == 1 )); then
        for file in "${!candidates[@]}"; do UBSM_LIBRARY=$file; done
      else
        echo "Cannot select libubsm_sdk under $sdk_root/lib or lib64; set UBSM_LIBRARY to its exact path." >&2
        return 2
      fi
    fi
  fi
  for file in "$UBSM_INCLUDE_DIR/ubs_mem.h" "$UBSM_INCLUDE_DIR/ubs_mem_def.h" "$UBSM_LIBRARY"; do
    if [[ ! -r $file ]]; then echo "Missing UBSM SDK file: $file" >&2; return 2; fi
  done
  export UBSM_LIBRARY
}

if [[ -n ${UB_IMAGE:-} && -n ${UB_CONTAINER:-} ]]; then
  echo 'Use UB_IMAGE or UB_CONTAINER, not both.' >&2; exit 2
fi

# Re-enter the same script inside an already configured UB container. Preserve
# its SDK/runtime environment unless the caller explicitly overrides it.
if [[ -n ${UB_CONTAINER:-} || -n ${UB_IMAGE:-} ]]; then
  env_args=(-e UB_CONTAINER= -e UB_IMAGE=)
  for name in UBSM_PROVIDER_HOST UBSM_PROVIDER_NUMA UBSM_INCLUDE_DIR UBSM_LIBRARY \
      LD_LIBRARY_PATH UBSM_RUNTIME_LIBRARY_DIR UB_SDK_TEST_MODE HCOM_CONNECTION_RETRY_TIMES CASES RECORDS OPS SIZE_MB ATOMIC_SIZE_MB JOBS TIMEOUT OUTPUT; do
    if [[ -v $name ]]; then env_args+=(-e "$name=${!name}"); fi
  done
  if [[ -n ${UB_IMAGE:-} ]]; then
    sdk_root=${UB_SDK_ROOT:-/usr/local/ubs_mem}
    if [[ ! -d $sdk_root || $sdk_root != /* ]]; then
      echo "UB_SDK_ROOT must be an existing absolute directory: $sdk_root" >&2; exit 2
    fi
    if [[ -z ${1:-${UBSM_PROVIDER_HOST:-}} ]]; then
      echo 'Specify the memory provider UB topology hostname; see --help.' >&2; exit 2
    fi
    # Compilation needs source and SDK files only. Standalone smoke tests
    # still need service sockets and dynamically created OBMM device nodes.
    run_args=(--rm --user "$(id -u):$(id -g)"
      -v "$ROOT:$ROOT" -w "$ROOT" -v "$sdk_root:$sdk_root:ro")
    if [[ -n ${UB_SDK_TEST_MODE:-} ]]; then
      run_args+=(--network host --uts host --device-cgroup-rule 'c *:* rw'
        -v /dev:/dev -v /run:/run:ro)
    fi
    for gid in $(id -G); do run_args+=(--group-add "$gid"); done
    resolve_sdk
    # Explicit headers/library may be outside the SDK root. Mount just the
    # selected files/directories, not the host's complete system library tree.
    include_real=$(readlink -f -- "$UBSM_INCLUDE_DIR")
    library_real=$(readlink -f -- "$UBSM_LIBRARY")
    run_args+=(-v "$include_real:$include_real:ro" -v "$library_real:$library_real:ro")
    env_args+=(-e "UBSM_INCLUDE_DIR=$include_real" -e "UBSM_LIBRARY=$library_real")
    # Copy the full resolved vendor dependency closure into an isolated bundle.
    # Do not overlay host glibc/GCC libraries onto the compilation image.
    mkdir -p "$ROOT/build/ub-runtime"
    bundle_parent=$(mktemp -d "$ROOT/build/ub-runtime/run-XXXXXX")
    bundle="$bundle_parent/lib"
    extra_args=()
    IFS=: read -r -a extra_libs <<< "${UB_EXTRA_LIBRARIES:-}"
    for lib in "${extra_libs[@]}"; do
      [[ -z $lib ]] || extra_args+=(--extra "$lib")
    done
    LD_LIBRARY_PATH="$(dirname -- "$library_real"):$sdk_root/lib:$sdk_root/lib64${UB_EXTRA_LIBRARY_DIRS:+:$UB_EXTRA_LIBRARY_DIRS}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      python3 "$ROOT/tests/two-process-ycsb/collect_ub_runtime.py" \
        --library "$library_real" --output "$bundle" "${extra_args[@]}"
    run_args+=(-v "$bundle:$bundle:ro")
    env_args+=(-e "UBSM_RUNTIME_LIBRARY_DIR=$bundle")
    if [[ -n ${UB_DOCKER_ARGS_FILE:-} ]]; then
      [[ -r $UB_DOCKER_ARGS_FILE ]] || { echo "Cannot read $UB_DOCKER_ARGS_FILE" >&2; exit 2; }
      while IFS= read -r arg || [[ -n $arg ]]; do
        [[ -z $arg || $arg == \#* ]] || run_args+=("$arg")
      done < "$UB_DOCKER_ARGS_FILE"
    fi
    if [[ -n ${UB_SDK_TEST_MODE:-} ]]; then
      echo "Creating temporary SDK smoke-test container from image: $UB_IMAGE"
      exec docker run "${run_args[@]}" "${env_args[@]}" --entrypoint /bin/bash "$UB_IMAGE" \
        "$ROOT/tests/two-process-ycsb/run_ub_single.sh" "$@"
    fi
    output=${OUTPUT:-build/ub-single-$(date +%Y%m%d-%H%M%S)-$$}
    if [[ $output != /* ]]; then output="$ROOT/$output"; fi
    if [[ -e $output || -e ${output}-build ]]; then
      echo "Output already exists: $output or ${output}-build" >&2; exit 2
    fi
    echo "Compiling in Docker: $UB_IMAGE; build logs: ${output}-build"
    docker run "${run_args[@]}" "${env_args[@]}" \
      -e UB_SINGLE_BUILD_ONLY=1 -e "OUTPUT=${output}-build" \
      --entrypoint /bin/bash "$UB_IMAGE" \
      "$ROOT/tests/two-process-ycsb/run_ub_single.sh" "$@"
    echo "Compilation finished; running UBSM admin and workers on host: $(uname -n)"
    # Re-enter on the host without the container's dependency bundle/environment.
    exec env -u UB_IMAGE -u UB_CONTAINER -u UB_SINGLE_BUILD_ONLY \
      UB_SINGLE_SKIP_BUILD=1 "OUTPUT=$output" \
      bash "$ROOT/tests/two-process-ycsb/run_ub_single.sh" "$@"
  fi
  exec docker exec "${env_args[@]}" -w "$ROOT" "$UB_CONTAINER" \
    bash "$ROOT/tests/two-process-ycsb/run_ub_single.sh" "$@"
fi

provider=${1:-${UBSM_PROVIDER_HOST:-}}
if [[ -z $provider ]]; then
  echo 'Specify the memory provider UB topology hostname as argument 1 or UBSM_PROVIDER_HOST; see --help.' >&2
  exit 2
fi
resolve_sdk
export LD_LIBRARY_PATH="${UBSM_RUNTIME_LIBRARY_DIR:+$UBSM_RUNTIME_LIBRARY_DIR:}$(dirname -- "$UBSM_LIBRARY")${UB_EXTRA_LIBRARY_DIRS:+:$UB_EXTRA_LIBRARY_DIRS}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
echo "UBSM headers: $UBSM_INCLUDE_DIR"
echo "UBSM library: $UBSM_LIBRARY"
# Show missing transitive libraries before starting a build with redirected logs.
if ! sdk_deps=$(ldd -r "$UBSM_LIBRARY" 2>&1); then
  printf '%s\n' "$sdk_deps" >&2
  echo 'Cannot load UBSM SDK in this environment; check its architecture and dependencies.' >&2
  exit 2
fi
if [[ $sdk_deps == *"not found"* || $sdk_deps == *"undefined symbol"* ]]; then
  printf '%s\n' "$sdk_deps" >&2
  echo 'Unresolved UBSM dependency/symbol above; verify matching SDK dependency versions. Host glibc/GCC are intentionally not copied.' >&2
  exit 2
fi
export HCOM_CONNECTION_RETRY_TIMES=${HCOM_CONNECTION_RETRY_TIMES:-2}
cd "$ROOT"
output=${OUTPUT:-build/ub-single-$(date +%Y%m%d-%H%M%S)-$$}
if [[ $output != /* ]]; then output="$ROOT/$output"; fi
if [[ -n ${UB_SDK_TEST_MODE:-} ]]; then
  [[ ${TIMEOUT:-60} =~ ^[1-9][0-9]*$ ]] || { echo 'TIMEOUT must be positive seconds' >&2; exit 2; }
  mkdir -p "$output"
  link_args=(-Wl,-rpath,"$(dirname -- "$UBSM_LIBRARY")")
  if [[ -n ${UBSM_RUNTIME_LIBRARY_DIR:-} ]]; then
    link_args+=(-Wl,-rpath,"$UBSM_RUNTIME_LIBRARY_DIR" -Wl,-rpath-link,"$UBSM_RUNTIME_LIBRARY_DIR")
  fi
  echo "Standalone SDK test; logs: $output"
  "${CXX:-c++}" -std=c++17 -O0 -g -I"$UBSM_INCLUDE_DIR" \
    "$ROOT/tests/ub-api/sdk_smoke.cc" "$UBSM_LIBRARY" "${link_args[@]}" \
    -o "$output/ub_sdk_smoke" 2>&1 | tee "$output/build.log"
  set +e
  timeout --signal=TERM --kill-after=5 "${TIMEOUT:-60}" \
    "$output/ub_sdk_smoke" "$UB_SDK_TEST_MODE" "$provider" "${SIZE_MB:-128}" \
    2>&1 | tee "$output/run.log"
  statuses=("${PIPESTATUS[@]}")
  set -e
  rc=${statuses[0]}
  if (( rc == 124 || rc == 137 )); then
    echo "SDK test timed out/killed. Last BEGIN without END in $output/run.log identifies the blocked stage." >&2
    echo "If CREATE was printed, the named UB object may remain; confirm all users stopped before cleanup." >&2
  fi
  if (( rc == 0 )); then rc=${statuses[1]}; fi
  exit "$rc"
fi
read -r -a cases <<< "${CASES:-bwtree-on clevelhash-on}"
runner_args=()
if [[ ${UB_SINGLE_BUILD_ONLY:-0} == 1 ]]; then
  runner_args+=(--build-only)
elif [[ ${UB_SINGLE_SKIP_BUILD:-0} == 1 ]]; then
  runner_args+=(--skip-build)
  # Check selected cases before the first UB allocation. Host glibc, libstdc++,
  # memkind and other dependencies must support the container-built binaries.
  for case in "${cases[@]}"; do
    for binary in bench ub_admin; do
      executable="$ROOT/build/two-ycsb/$case-ubsm-split/$binary"
      if ! deps=$(ldd -r "$executable" 2>&1) ||
          [[ $deps == *"not found"* || $deps == *"undefined symbol"* ]]; then
        printf '%s\n' "$deps" >&2
        echo "Host cannot load $executable; use a host-compatible build image or install matching host dependencies. No UB objects have been created." >&2
        exit 2
      fi
    done
  done
fi
if [[ ${UB_SINGLE_BUILD_ONLY:-0} == 1 ]]; then
  echo "Build only: no UBSM initialization or workers in this container."
else
  echo "UBSM: workers_host=$(uname -n), memory_provider=$provider, provider_numa=${UBSM_PROVIDER_NUMA:-4294967295}, threads=${2:-2}/${3:-${2:-2}}"
fi
echo "Mapping policy: CACHE data + NONCACHE atomics; atomic arena=${ATOMIC_SIZE_MB:-512} MiB."
echo "Build logs, worker logs and results.json: $output"
trap 'rc=$?; if (( rc != 0 )); then echo "Failed; inspect logs in $output. If a UB object was created, confirm workers stopped before manual cleanup." >&2; fi' EXIT
python3 tests/two-process-ycsb/run.py "${runner_args[@]}" \
  --cases "${cases[@]}" --memory ub --ub-api ubsm --ub-layout split \
  --atomic-size-mb "${ATOMIC_SIZE_MB:-512}" \
  --ub-library "$UBSM_LIBRARY" --ub-include "$UBSM_INCLUDE_DIR" \
  --provider-host "$provider" --provider-numa "${UBSM_PROVIDER_NUMA:-4294967295}" \
  --shared-path ub-single --threads "${2:-2}" "${3:-${2:-2}}" \
  --records "${RECORDS:-10000}" --ops "${OPS:-10000}" \
  --size-mb "${SIZE_MB:-1024}" --mapping-bits 16 \
  --jobs "${JOBS:-8}" --timeout "${TIMEOUT:-300}" --output "$output"

#!/bin/sh
set -e

cd "$(dirname "$0")"
# The original entry also selects the coordinated CXL/UB process backend.
case "${YCSB_BACKEND:-}" in
    cxl) YCSB_CXL_TWO_PROCESS=1 ;;
    ub) YCSB_CXL_TWO_PROCESS=0; YCSB_UB_TWO_HOST=1; SHM_UB_API=${SHM_UB_API:-ubsm} ;;
    "") ;;
    *) echo "YCSB_BACKEND must be cxl or ub" >&2; exit 2 ;;
esac
# Default to the three requested SDK modes.
if [ "$#" -eq 0 ]; then set -- opt noopt cc; fi

for variant in "$@"; do
    # normalize to lowercase (accept NOCC, nocc, etc.)
    lower_variant=$(echo "$variant" | tr '[:upper:]' '[:lower:]')
    case "$lower_variant" in
        opt) lower_variant=nocc ;;
        noopt) lower_variant=nocc_no_opt ;;
        rpc) lower_variant=cc_nocc_mq ;;
    esac
    build_dir="build_$lower_variant"
    if [ "${YCSB_UB_TWO_HOST:-0}" = 1 ]; then build_dir="${build_dir}_ub"; fi
    if [ "${YCSB_CXL_TWO_PROCESS:-0}" = 1 ]; then build_dir="build_${lower_variant}_cxl"; fi
    build_dir=${YCSB_BUILD_DIR:-$build_dir}
    mkdir -p "$build_dir" || { echo "Failed to create directory $build_dir"; exit 1; }
    # Environment selection overrides a cached API; SDK paths remain explicit.
    set --
    [ -z "${SHM_UB_API:-}" ] || set -- "$@" "-DUB_API=$SHM_UB_API" "-DWITH_UB=OFF"
    [ -z "${UBSM_INCLUDE_DIR:-}" ] || set -- "$@" "-DUBSM_INCLUDE_DIR=$UBSM_INCLUDE_DIR"
    [ -z "${UBSM_LIBRARY:-}" ] || set -- "$@" "-DUBSM_LIBRARY=$UBSM_LIBRARY"
    [ -z "${UB_MEMLINK_LIBRARY:-}" ] || set -- "$@" "-DUB_MEMLINK_LIBRARY=$UB_MEMLINK_LIBRARY"
    # Always reset process mode when reusing a build directory.
    set -- "$@" -DYCSB_UB_TWO_HOST=OFF -DYCSB_CXL_TWO_PROCESS=OFF
    [ -z "${YCSB_INDEXES:-}" ] || set -- "$@" "-DYCSB_INDEXES=$YCSB_INDEXES"
    if [ "${YCSB_UB_TWO_HOST:-0}" = 1 ] || [ "${YCSB_CXL_TWO_PROCESS:-0}" = 1 ]; then
        set -- "$@" -DYCSB_UB_TWO_HOST=ON "-DYCSB_INDEXES=${YCSB_INDEXES:-bwtree}" "-DBWTREE_MAPPING_TABLE_BITS=${MAPPING_BITS:-24}"
    fi
    if [ "${YCSB_CXL_TWO_PROCESS:-0}" = 1 ]; then
        set -- "$@" -DYCSB_CXL_TWO_PROCESS=ON -DUB_API=none -DWITH_UB=OFF
    fi
    [ "${YCSB_BUILD_READONLY_RPC_CONTRACT:-0}" != 1 ] || set -- "$@" -DYCSB_BUILD_READONLY_RPC_CONTRACT=ON
    cmake -B "$build_dir" "$@" -DVARIANT="$lower_variant" -DBUILD_EXTRA_INDEXES=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || { echo "CMake configuration failed for $variant ($lower_variant)"; exit 1; }
    cmake --build "$build_dir" --parallel "${JOBS:-8}" || { echo "Make failed for $variant"; exit 1; }
    [ "${YCSB_UPDATE_SYMLINKS:-1}" = 1 ] || continue
    ln -sf "$build_dir/ycsbc_$lower_variant" ycsbc || { echo "Failed to create symlink for $variant"; exit 1; }
    ln -sf "$build_dir" build || { echo "Failed to create symlink for build_$lower_variant"; exit 1; }
    ln -sf "$(cd "$build_dir" && pwd)/compile_commands.json" ../../compile_commands.json || echo "Warning: failed to create compile_commands.json symlink for $variant"
done

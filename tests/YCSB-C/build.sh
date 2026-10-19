#!/bin/sh
set -e

cd "$(dirname "$0")"
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
    mkdir -p "$build_dir" || { echo "Failed to create directory $build_dir"; exit 1; }
    cmake -B "$build_dir" -DVARIANT="$lower_variant" -DBUILD_EXTRA_INDEXES=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || { echo "CMake configuration failed for $variant ($lower_variant)"; exit 1; }
    cmake --build "$build_dir" --parallel "${JOBS:-8}" || { echo "Make failed for $variant"; exit 1; }
    ln -sf "$build_dir/ycsbc_$lower_variant" ycsbc || { echo "Failed to create symlink for $variant"; exit 1; }
    ln -sf "$build_dir" build || { echo "Failed to create symlink for build_$lower_variant"; exit 1; }
    ln -sf "$(pwd)/$build_dir/compile_commands.json" ../../compile_commands.json || echo "Warning: failed to create compile_commands.json symlink for $variant"
done

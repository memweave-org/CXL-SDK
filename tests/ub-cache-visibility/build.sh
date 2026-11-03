#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
: "${UBSM_INCLUDE_DIR:?Set UBSM_INCLUDE_DIR to the vendor SDK include directory}"
: "${UBSM_LIBRARY:?Set UBSM_LIBRARY to the absolute vendor libubsm_sdk.so path}"
output=${1:-"$ROOT/build/ub-cache-visibility/worker"}
mkdir -p -- "$(dirname -- "$output")"
"${CXX:-c++}" -std=c++17 -O2 -g -Wall -Wextra -Werror \
  -I"$UBSM_INCLUDE_DIR" "$ROOT/tests/ub-cache-visibility/worker.cc" \
  "$UBSM_LIBRARY" -Wl,-rpath,"$(dirname -- "$UBSM_LIBRARY")" -o "$output"

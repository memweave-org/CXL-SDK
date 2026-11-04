#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/ub-bwtree-ycsb"
: "${UBSM_INCLUDE_DIR:?}"
: "${UBSM_LIBRARY:?}"
output=${1:?output path required}
mkdir -p "$(dirname -- "$output")/bwtree"
staged="$(dirname -- "$output")/bwtree"
cp "$ROOT"/ds/BwTree/src/*.h "$staged/"
python3 "$HERE/prepare_source.py" "$ROOT/ds/BwTree/src/bwtree.h" "$staged/bwtree.h"
# Compile the copied header consistently in all translation units.
cp "$ROOT/ds/BwTree/src/bwtree.cpp" "$staged/"
c++ -std=c++17 -O3 -g -fno-pie -no-pie -pthread -UNDEBUG \
  -DBWTREE_MAPPING_TABLE_BITS="${MAPPING_BITS:-20}" -DSHM_DISABLE_CAS128 \
  -I"$HERE" -I"$staged" -I"$ROOT/shm-lib/include" -I"$ROOT/tests/YCSB-C/include" \
  -I"$UBSM_INCLUDE_DIR" -include "$HERE/cache.h" \
  "$HERE/worker.cc" "$HERE/arena.cc" "$staged/bwtree.cpp" \
  "$ROOT/shm-lib/utils/sim_id.cc" "$ROOT/tests/YCSB-C/core/core_workload.cc" \
  "$ROOT/tests/YCSB-C/core/utils.cc" "$UBSM_LIBRARY" \
  -Wl,-rpath-link,"$(dirname -- "$UBSM_LIBRARY")" -latomic -o "$output"

#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
: "${BUILD_IMAGE:=shm-pcc-sdk:ub-glibc2.39-gcc12.3.1}"
: "${SDK_ROOT:=/usr/local/ubs_mem}"
: "${OPT:=1}"
: "${YCSB_INDEXES:=bwtree}"
ROOT=$(cd ../../.. && pwd)
SDK_LIBRARY=${SDK_LIBRARY:-$SDK_ROOT/lib/libubsm_sdk.so}
if [[ ! -f $SDK_LIBRARY && -f $SDK_ROOT/lib64/libubsm_sdk.so ]]; then SDK_LIBRARY=$SDK_ROOT/lib64/libubsm_sdk.so; fi
export LD_LIBRARY_PATH="$(dirname -- "$SDK_LIBRARY"):$SDK_ROOT/lib:$SDK_ROOT/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
out=${1:?output directory required}
mkdir -p "$out"
out=$(cd "$out" && pwd)
if [[ ${YCSB_BACKEND:-ub} == cxl ]]; then
  variant=nocc
  [[ $OPT != 0 ]] || variant=nocc_no_opt
  [[ ${YCSB_RPC_BUILD:-0} != 1 ]] || variant=cc_nocc_mq
  YCSB_CXL_TWO_PROCESS=1 SHM_UB_API=none YCSB_BUILD_DIR="$out/build" \
    YCSB_UPDATE_SYMLINKS=0 YCSB_INDEXES="$YCSB_INDEXES" JOBS="${JOBS:-2}" \
    bash ../build.sh "$variant"
  cp "$out/build/ycsbc_$variant" "$out/ycsbc"
  python3 "$ROOT/tests/two-process-ycsb/collect_ub_runtime.py" --library "$out/ycsbc" --output "$out/lib"
  rm "$out/lib/ycsbc"
  exit 0
fi
context="$out/context"
mkdir -p "$context/src/tests/YCSB-C" "$context/src/tests/two-process-ycsb" "$context/src/shm-lib" "$context/src/ds/BwTree"
cp -Lr "$SDK_ROOT/include" "$context/include"
cp Dockerfile "$context/"
cp -r "$ROOT/cmake" "$context/src/"
for dir in include clstale msg shm utils connection; do cp -r "$ROOT/shm-lib/$dir" "$context/src/shm-lib/"; done
cp "$ROOT/shm-lib/CMakeLists.txt" "$context/src/shm-lib/"
cp -r "$ROOT/ds/BwTree/src" "$ROOT/ds/BwTree/BTree" "$context/src/ds/BwTree/"
mkdir -p "$context/src/ds/BwTree/benchmark"
cp -r "$ROOT/ds/BwTree/benchmark/art" "$context/src/ds/BwTree/benchmark/"
cp "$ROOT/ds/BwTree/CMakeLists.txt" "$context/src/ds/BwTree/"
if [[ ";$YCSB_INDEXES;" == *';clevel-hash;'* ]]; then
  cp -r "$ROOT/ds/ClevelHash" "$context/src/ds/"
fi
if [[ ";$YCSB_INDEXES;" == *';sherman;'* ]]; then
  cp -r "$ROOT/ds/Sherman" "$ROOT/ds/cxl_transport" "$context/src/ds/"
  mkdir -p "$context/src/drivers/uncached_ram"
  cp "$ROOT/drivers/uncached_ram/uncached_ram_uapi.h" "$context/src/drivers/uncached_ram/"
fi
for dir in include core db follower; do cp -r "../$dir" "$context/src/tests/YCSB-C/"; done
cp ../CMakeLists.txt ../build.sh ../ycsbc.cc "$context/src/tests/YCSB-C/"
cp "$ROOT/tests/two-process-ycsb/collect_ub_runtime.py" "$context/src/tests/two-process-ycsb/"
python3 "$ROOT/tests/two-process-ycsb/collect_ub_runtime.py" --library "$SDK_LIBRARY" --output "$context/lib"
cp -L "$SDK_LIBRARY" "$context/lib/libubsm_sdk.so"
image="ycsb-framework-ub:$(id -u)-$$"
container=''
cleanup() {
  [[ -z $container ]] || docker rm "$container" >/dev/null
  docker image rm "$image" >/dev/null 2>&1 || true
}
trap cleanup EXIT
docker build --build-arg "BUILD_IMAGE=$BUILD_IMAGE" --build-arg "OPT=$OPT" --build-arg "YCSB_RPC_BUILD=${YCSB_RPC_BUILD:-0}" --build-arg "YCSB_INDEXES=$YCSB_INDEXES" --build-arg "JOBS=${JOBS:-2}" -t "$image" "$context" 2>&1 | tee "$out/build.log"
container=$(docker create "$image" /bin/true)
docker cp "$container:/out/." "$out/"
chmod +x "$out/ycsbc"

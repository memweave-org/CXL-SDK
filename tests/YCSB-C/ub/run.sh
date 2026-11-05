#!/usr/bin/env bash
# Build the original YCSB-C executable in Docker, execute on the physical host.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source "${YCSB_UB_CONFIG:-./config.sh}"
[[ $A_WRITABLE == 0 ]] || { echo "A is always read-only; A_WRITABLE must be 0" >&2; exit 2; }
case_name=${YCSB_UB_CASE:-bwtree}
suite=${YCSB_UB_SUITE:-0}
db=bwtree; rpc=0; sherman_access=cache
case "$case_name" in
  bwtree) if [[ $suite == 1 ]]; then OPT=0; fi;;
  bwtree+opt) OPT=1;;
  clevelhash) db=clevelhash; OPT=0;;
  clevelhash+opt) db=clevelhash; OPT=1;;
  clevelhash+rpc) db=clevelhash; OPT=0; rpc=1;;
  bwtree+rpc) OPT=0; rpc=1;;
  sherman) db=sherman; OPT=0;;
  sherman+nc-direct) db=sherman; OPT=0; sherman_access=nc-direct;;
  sherman+nc-memcpy) db=sherman; OPT=0; sherman_access=nc-memcpy;;
  *) echo "Unknown UB case: $case_name" >&2; exit 2;;
esac
case $OPT in 0|1) ;; *) echo 'OPT must be 0 or 1' >&2; exit 2;; esac
role=${YCSB_UB_ROLE:-}
if [[ -z $role ]]; then
role=$(python3 - "$A_IP" "$B_IP" <<'PYROLE'
import json, subprocess, sys
interfaces=json.loads(subprocess.check_output(['ip','-j','-4','addr','show'],text=True))
addresses={a['local'] for interface in interfaces for a in interface.get('addr_info',[])}
roles=[r for r,ip in zip(('A','B'),sys.argv[1:]) if ip in addresses]
if len(roles)!=1:sys.exit('无法按本机 IP 唯一识别 A/B，请检查 ub/config.sh')
print(roles[0])
PYROLE
)
fi
[[ $role == A || $role == B ]] || { echo "Invalid role: $role" >&2; exit 2; }
out=${YCSB_UB_OUTPUT:-$(mktemp -d "$PWD/result-$role-XXXXXX")}
mkdir -p "$out"
out=$(cd "$out" && pwd)
if [[ -n ${YCSB_UB_BUNDLE:-} ]]; then
  ln -s "$(realpath "$YCSB_UB_BUNDLE/ycsbc")" "$out/ycsbc"
  ln -s "$(realpath "$YCSB_UB_BUNDLE/lib")" "$out/lib"
else
case "$db" in
  bwtree) indexes=bwtree;;
  clevelhash) indexes=clevel-hash;;
  sherman) indexes=sherman;;
esac
BUILD_IMAGE="$BUILD_IMAGE" SDK_ROOT="$SDK_ROOT" SDK_LIBRARY="${SDK_LIBRARY:-}" OPT="$OPT" JOBS="${JOBS:-2}" YCSB_INDEXES="$indexes" bash ./build.sh "$out"
fi
export LD_LIBRARY_PATH="$out/lib:$SDK_ROOT/lib:$SDK_ROOT/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
deps=$(ldd -r "$out/ycsbc" 2>&1) || { echo "$deps"; exit 1; }
if [[ $deps == *'not found'* || $deps == *'undefined symbol'* ]]; then echo "$deps"; exit 1; fi
echo "物理机 $(hostname)，角色 $role，原框架 ycsbc，case=$case_name OPT=$OPT RPC=$rpc，NT backend=UB（直接访问 NC），日志 $out"
if [[ $role == A ]]; then threads=$THREADS_A; cpu=${CPU_A:-}; else threads=$THREADS_B; cpu=${CPU_B:-}; fi
cpu_args=(); if [[ -n $cpu ]]; then cpu_args=(--cpu "$cpu"); fi
spec=$SPEC
[[ $spec == /* ]] || spec="$PWD/$spec"
suite_args=()
if [[ $case_name != bwtree || $suite == 1 ]]; then
  suite_args=(--suite --case "$case_name" --db "$db" --rpc "$rpc" --a-writable "$A_WRITABLE" --seed "$SEED" --sherman-access "$sherman_access")
fi
if [[ ${YCSB_UB_FIXTURE:-0} == 1 ]]; then suite_args+=(--fixture); fi
python3 ./peer.py --role "$role" --peer "$B_IP" --port "$PORT" --provider "$PROVIDER_B" \
  --binary "$out/ycsbc" --output "$out" --threads "$threads" --records "$RECORDS" --ops "$OPS" \
  --size-mb "$SIZE_MB" --nc-size-mb "$NC_SIZE_MB" --spec "$spec" --ini "$PWD/config.ini" --expected-opt "$OPT" "${cpu_args[@]}" "${suite_args[@]}"

#!/usr/bin/env bash
# Build the original YCSB-C executable in Docker, execute on the physical host.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source "${YCSB_UB_CONFIG:-./config.sh}"
case $OPT in 0|1) ;; *) echo 'OPT must be 0 or 1' >&2; exit 2;; esac
role=$(python3 - "$A_IP" "$B_IP" <<'PYROLE'
import json, subprocess, sys
interfaces=json.loads(subprocess.check_output(['ip','-j','-4','addr','show'],text=True))
addresses={a['local'] for interface in interfaces for a in interface.get('addr_info',[])}
roles=[r for r,ip in zip(('A','B'),sys.argv[1:]) if ip in addresses]
if len(roles)!=1:sys.exit('无法按本机 IP 唯一识别 A/B，请检查 ub/config.sh')
print(roles[0])
PYROLE
)
out=$(mktemp -d "$PWD/result-$role-XXXXXX")
BUILD_IMAGE="$BUILD_IMAGE" SDK_ROOT="$SDK_ROOT" SDK_LIBRARY="${SDK_LIBRARY:-}" OPT="$OPT" JOBS="${JOBS:-2}"   bash ./build.sh "$out"
export LD_LIBRARY_PATH="$out/lib:$SDK_ROOT/lib:$SDK_ROOT/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
deps=$(ldd -r "$out/ycsbc" 2>&1) || { echo "$deps"; exit 1; }
if [[ $deps == *'not found'* || $deps == *'undefined symbol'* ]]; then echo "$deps"; exit 1; fi
echo "物理机 $(hostname)，角色 $role，原框架 ycsbc，OPT=$OPT，NT backend=UB（直接访问 NC），日志 $out"
if [[ $role == A ]]; then threads=$THREADS_A; cpu=${CPU_A:-}; else threads=$THREADS_B; cpu=${CPU_B:-}; fi
cpu_args=(); if [[ -n $cpu ]]; then cpu_args=(--cpu "$cpu"); fi
python3 ./peer.py --role "$role" --peer "$B_IP" --port "$PORT" --provider "$PROVIDER_B" \
  --binary "$out/ycsbc" --output "$out" --threads "$threads" --records "$RECORDS" --ops "$OPS" \
  --size-mb "$SIZE_MB" --nc-size-mb "$NC_SIZE_MB" --spec "$PWD/$SPEC" --ini "$PWD/config.ini" "${cpu_args[@]}"

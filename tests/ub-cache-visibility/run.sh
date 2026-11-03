#!/usr/bin/env bash
# Docker builds only; the worker and control channel execute on the physical host.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source ./config.sh
role=$(python3 - "$A_IP" "$B_IP" <<'PYROLE'
import json, subprocess, sys
interfaces = json.loads(subprocess.check_output(['ip', '-j', '-4', 'addr', 'show'], text=True))
addresses = {a['local'] for interface in interfaces for a in interface.get('addr_info', [])}
roles = [role for role, address in zip(('A', 'B'), sys.argv[1:]) if address in addresses]
if len(roles) != 1:
    sys.exit('无法按本机 IP 唯一识别 A/B，请检查 config.sh 中的 IP 是否属于本机网卡。')
print(roles[0])
PYROLE
)
ROOT=$(cd ../.. && pwd)
SDK_LIBRARY=${SDK_LIBRARY:-$SDK_ROOT/lib/libubsm_sdk.so}
if [[ ! -f $SDK_LIBRARY && -f $SDK_ROOT/lib64/libubsm_sdk.so ]]; then
  SDK_LIBRARY=$SDK_ROOT/lib64/libubsm_sdk.so
fi
export LD_LIBRARY_PATH="$(dirname -- "$SDK_LIBRARY"):$SDK_ROOT/lib:$SDK_ROOT/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
out=$(mktemp -d "$PWD/result-$role-XXXXXX")
context="$out/context"
mkdir -p "$context"
cp -Lr "$SDK_ROOT/include" "$context/include"
cp worker.cc Dockerfile "$context/"
python3 "$ROOT/tests/two-process-ycsb/collect_ub_runtime.py" \
  --library "$SDK_LIBRARY" --output "$context/lib"
cp -L "$SDK_LIBRARY" "$context/lib/libubsm_sdk.so"
image="ub-cache-micro:$(id -u)-${role,,}-$$"
container=''
cleanup() {
  [[ -z $container ]] || docker rm "$container" >/dev/null
  docker image rm "$image" >/dev/null 2>&1 || true
}
trap cleanup EXIT
# No SDK initialization or test runs inside Docker.
docker build --build-arg "BUILD_IMAGE=$BUILD_IMAGE" -t "$image" "$context" 2>&1 | tee "$out/build.log"
container=$(docker create "$image" /bin/true)
docker cp "$container:/worker" "$out/worker"
chmod +x "$out/worker"
deps=$(ldd -r "$out/worker" 2>&1) || { echo "$deps"; exit 1; }
if [[ $deps == *'not found'* || $deps == *'undefined symbol'* ]]; then
  echo "$deps"; echo '编译镜像与物理机运行库不兼容，或 SDK 依赖缺失。' >&2; exit 1
fi
echo "物理机 $(hostname)，角色 $role，日志 $out"
cpu_args=()
if [[ -n ${CPU:-} ]]; then cpu_args=(--cpu "$CPU"); fi
python3 ./peer.py --role "$role" --peer "$B_IP" --port "$PORT" \
  --provider "$PROVIDER_B" --binary "$out/worker" --output "$out" "${cpu_args[@]}"

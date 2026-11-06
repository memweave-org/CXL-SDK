#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd)
IMAGE="shm-pcc-sdk:ub-glibc2.39-gcc12.3.1"
CONTAINER="ycsb-build-$(date +%s)-$$"
DETACH=false
RM=false
BUILD=false
BUILD_ARGS=()

show_help() {
    echo "Usage: $0 [-b] [-i image] [-c container] [-d] [-r] [variants...]"
    echo "Build with glibc 2.39 / GCC 12.3.1; no variants opens a shell."
    echo "-b rebuild image; -d detach; -r remove container on exit"
    echo "Platform defaults to the Docker host; set DOCKER_DEFAULT_PLATFORM for another architecture."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help; exit 0 ;;
        -i|-c)
            [[ $# -ge 2 && -n "$2" ]] || { echo "Missing value for $1" >&2; exit 2; }
            if [[ "$1" == -i ]]; then IMAGE="$2"; else CONTAINER="$2"; fi
            shift 2 ;;
        -d) DETACH=true; shift ;;
        -r) RM=true; shift ;;
        -b) BUILD=true; shift ;;
        -*) echo "Unknown option: $1" >&2; exit 2 ;;
        *) BUILD_ARGS+=("$1"); shift ;;
    esac
done

if [[ "$BUILD" == true ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    PROXY_ARGS=()
    for proxy_name in http_proxy https_proxy no_proxy HTTP_PROXY HTTPS_PROXY NO_PROXY; do
        if [[ -v "$proxy_name" ]]; then
            PROXY_ARGS+=(--build-arg "$proxy_name")
        fi
    done
    docker build --network=host "${PROXY_ARGS[@]}" --build-arg MEMKIND_FROM_SOURCE=1 \
        -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile.openEuler" "$SCRIPT_DIR"
fi

# Check custom and pre-existing images too; tags alone do not establish versions.
docker run --rm --entrypoint /bin/bash "$IMAGE" -ec '
    test "$(getconf GNU_LIBC_VERSION)" = "glibc 2.39"
    test "$(gcc -dumpfullversion)" = "12.3.1"
    test "$(g++ -dumpfullversion)" = "12.3.1"
    getconf GNU_LIBC_VERSION
    gcc --version
'
RUN_ARGS=(--name "$CONTAINER" -v "$ROOT_DIR:$ROOT_DIR" -w "$SCRIPT_DIR")
# Global backend configuration is forwarded into the build container.
for name in SHM_UB_API UBSM_INCLUDE_DIR UBSM_LIBRARY UB_MEMLINK_LIBRARY; do
    [[ ! -v "$name" ]] || RUN_ARGS+=(-e "$name")
done
if [[ -n "${UB_SDK_ROOT:-}" ]]; then
    [[ -d "$UB_SDK_ROOT" ]] || { echo "UB_SDK_ROOT is not a directory" >&2; exit 2; }
    RUN_ARGS+=(-v "$UB_SDK_ROOT:$UB_SDK_ROOT:ro")
fi
[[ "$RM" == false ]] || RUN_ARGS+=(--rm)
if [[ "$DETACH" == true ]]; then
    RUN_ARGS+=(-d)
else
    RUN_ARGS+=(-i)
    [[ ! -t 0 || ! -t 1 ]] || RUN_ARGS+=(-t)
fi
if [[ ${#BUILD_ARGS[@]} -gt 0 ]]; then
    docker run "${RUN_ARGS[@]}" "$IMAGE" bash ./build.sh "${BUILD_ARGS[@]}"
else
    docker run "${RUN_ARGS[@]}" "$IMAGE" bash
fi

#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
export UB_MICRO_NC=1
# Start with the SDK example flag. Use noreplace to compare with current YCSB.
export NC_MAP_MODE=${NC_MAP_MODE:-shared}
export NC_SIZE_MB=${NC_SIZE_MB:-4}
exec bash ../run.sh

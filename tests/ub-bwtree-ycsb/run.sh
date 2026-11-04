#!/usr/bin/env bash
# Compatibility entry: run the original YCSB-C framework.
set -euo pipefail
exec bash "$(dirname -- "${BASH_SOURCE[0]}")/../YCSB-C/run.sh" "$@"

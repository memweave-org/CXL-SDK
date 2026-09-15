#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "Building STL..."

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure and build
cmake ..
make -j$(nproc)

echo "Build complete! Executables are in: ${BUILD_DIR}"
echo ""
echo "To run examples:"
echo "  cd ${BUILD_DIR}"
echo "  ./simple_demo"
echo "  ./multi_process_demo"

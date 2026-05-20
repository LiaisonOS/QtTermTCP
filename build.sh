#!/bin/bash
#
# Author  : Sylvain Deguire (VA2OPS)
# Date    : May 2026
# Purpose : Out-of-source build for QtTermTCP
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../QtTermTCP-build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
qmake "$SCRIPT_DIR/QtTermTCP.pro"
make -j$(nproc)

echo "Binary: $BUILD_DIR/QtTermTCP"

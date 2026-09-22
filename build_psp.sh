#!/bin/bash
set -e

export PSPDEV=/usr/local/pspdev
export PATH="$PSPDEV/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}"
BUILD_DIR="${SRC_DIR}/build/psp"

echo "=== OptiCraft Heritage Edition: Building for PSP ==="
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${SRC_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${PSPDEV}/psp/share/pspdev.cmake" \
    -DPLATFORM=PSP

cmake --build . --parallel $(nproc)

echo "=== Copying EBOOT.PBP to project root and bin/psp ==="
mkdir -p "${SRC_DIR}/bin/psp"
cp -f "${BUILD_DIR}/bin/psp/EBOOT.PBP" "${SRC_DIR}/bin/psp/EBOOT.PBP" 2>/dev/null || cp -f "${BUILD_DIR}/EBOOT.PBP" "${SRC_DIR}/bin/psp/EBOOT.PBP" 2>/dev/null || true
cp -f "${SRC_DIR}/bin/psp/EBOOT.PBP" "${SRC_DIR}/EBOOT.PBP" 2>/dev/null || true

echo "=== Build Complete! ==="
ls -lh "${SRC_DIR}/EBOOT.PBP"

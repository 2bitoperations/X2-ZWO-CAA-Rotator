#!/usr/bin/env bash
# setup.sh — install ZWO CAA library + build dependencies, then build test_caa
# Usage: bash setup.sh
set -euo pipefail

INDI3P="${HOME}/appinstall/indi-3rdparty"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="${SCRIPT_DIR}/setup.log"

# ── Logging setup ─────────────────────────────────────────────────────────────
# All output goes to both the terminal and the log file (appended with timestamp header).
exec > >(tee -a "${LOG}") 2>&1
echo ""
echo "========================================================"
echo "setup.sh  started  $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo "========================================================"

# ── 1. Verify indi-3rdparty checkout ─────────────────────────────────────────
if [[ ! -f "${INDI3P}/libasi/CAA_API.h" ]]; then
    echo "ERROR: ${INDI3P}/libasi/CAA_API.h not found."
    echo "Clone indi-3rdparty first:"
    echo "  git clone https://github.com/indilib/indi-3rdparty.git ${INDI3P}"
    exit 1
fi

ARCH="$(uname -m)"
case "${ARCH}" in
    aarch64) LIB_DIR="${INDI3P}/libasi/armv8" ;;
    armv7*)  LIB_DIR="${INDI3P}/libasi/armv7" ;;
    x86_64)  LIB_DIR="${INDI3P}/libasi/x64"   ;;
    *)
        echo "ERROR: Unsupported architecture: ${ARCH}"
        exit 1
        ;;
esac

LIB_BIN="${LIB_DIR}/libCAARotator.bin"
if [[ ! -f "${LIB_BIN}" ]]; then
    echo "ERROR: Prebuilt library not found at ${LIB_BIN}"
    exit 1
fi

# ── 2. Install build dependencies ────────────────────────────────────────────
echo "==> Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y \
    cmake build-essential pkg-config \
    libusb-1.0-0-dev \
    libhidapi-dev \
    libudev-dev

# ── 3. Install libCAARotator ─────────────────────────────────────────────────
echo "==> Installing libCAARotator (${ARCH})..."
sudo install -m 644 "${LIB_BIN}" /usr/local/lib/libCAARotator.so.1
sudo ln -sf /usr/local/lib/libCAARotator.so.1 /usr/local/lib/libCAARotator.so
sudo ldconfig

# ── 4. Install header ────────────────────────────────────────────────────────
echo "==> Installing CAA_API.h..."
sudo install -m 644 "${INDI3P}/libasi/CAA_API.h" /usr/local/include/CAA_API.h

# ── 5. Install udev rule ─────────────────────────────────────────────────────
echo "==> Installing udev rules..."
sudo install -m 644 "${INDI3P}/libasi/99-asi.rules" /etc/udev/rules.d/99-asi.rules
sudo udevadm control --reload-rules
sudo udevadm trigger

# ── 6. Build test harness ────────────────────────────────────────────────────
echo "==> Building test_caa..."
cd "${SCRIPT_DIR}"
g++ -std=c++17 -Wall -O2 \
    -I/usr/local/include \
    -o test_caa test_caa.cpp \
    -L/usr/local/lib -lCAARotator \
    -lusb-1.0 -ludev -lpthread

echo ""
echo "Build complete.  Run:  ./test_caa"
echo "Make sure the ZWO CAA is plugged in before running."
echo "Log appended to: ${LOG}"

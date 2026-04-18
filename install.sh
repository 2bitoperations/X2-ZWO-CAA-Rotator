#!/usr/bin/env bash
# install.sh — Install the ZWO CAA Rotator plugin for TheSkyX
#
# Usage:
#   ./install.sh              # install (build first with: make clean && make)
#   ./install.sh --uninstall  # remove installed files

set -euo pipefail

UNAME_S="$(uname -s)"
case "${UNAME_S}" in
    Darwin)
        TSX_APP="/Applications/TheSkyX Professional Edition.app"
        PLUGIN_DIR="${TSX_APP}/Contents/PlugIns/RotatorPlugIns"
        LIST_DIR="${TSX_APP}/Contents/Resources/Common/Miscellaneous Files"
        LIB_NAME="libx2caarotator.dylib"
        ;;
    Linux)
        TSX_HOME="${HOME}/TheSkyX"
        PLUGIN_DIR="${TSX_HOME}/Resources/Common/PlugIns64/RotatorPlugIns"
        LIST_DIR="${TSX_HOME}/Resources/Common/Miscellaneous Files"
        LIB_NAME="libx2caarotator.so"
        ;;
    *)
        echo "ERROR: Unsupported platform: ${UNAME_S}" >&2
        exit 1
        ;;
esac

LIST_FILE="rotatorlist ZWO CAA.txt"
UI_FILE="x2caarotator.ui"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--uninstall" ]]; then
    echo "Uninstalling ZWO CAA Rotator plugin..."
    rm -f "${PLUGIN_DIR}/${LIB_NAME}"      && echo "  Removed ${PLUGIN_DIR}/${LIB_NAME}"
    rm -f "${PLUGIN_DIR}/${UI_FILE}"       && echo "  Removed ${PLUGIN_DIR}/${UI_FILE}"
    rm -f "${LIST_DIR}/${LIST_FILE}"       && echo "  Removed ${LIST_DIR}/${LIST_FILE}"
    echo "Done."
    exit 0
fi

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
if [[ ! -d "${PLUGIN_DIR}" ]]; then
    echo "ERROR: Plugin directory not found:" >&2
    echo "  ${PLUGIN_DIR}" >&2
    echo "Is TheSkyX installed?  Does RotatorPlugIns exist?" >&2
    exit 1
fi

if [[ ! -d "${LIST_DIR}" ]]; then
    echo "ERROR: Miscellaneous Files directory not found:" >&2
    echo "  ${LIST_DIR}" >&2
    exit 1
fi

if [[ ! -f "${SCRIPT_DIR}/${LIB_NAME}" ]]; then
    echo "ERROR: ${LIB_NAME} not found — run 'make clean && make' first." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
echo "Installing plugin..."

cp "${SCRIPT_DIR}/${LIB_NAME}" "${PLUGIN_DIR}/${LIB_NAME}"
echo "  -> ${PLUGIN_DIR}/${LIB_NAME}"

cp "${SCRIPT_DIR}/${UI_FILE}" "${PLUGIN_DIR}/${UI_FILE}"
echo "  -> ${PLUGIN_DIR}/${UI_FILE}"

cp "${SCRIPT_DIR}/${LIST_FILE}" "${LIST_DIR}/${LIST_FILE}"
echo "  -> ${LIST_DIR}/${LIST_FILE}"

echo ""
echo "Installation complete."
echo "Restart TheSkyX and select 'ZWO CAA Rotator' from the Rotator device list."

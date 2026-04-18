#!/usr/bin/env bash
# install.sh — Install the ZWO CAA Rotator plugin for TheSkyX
#
# Usage:
#   ./install.sh              # install (build first with: make clean && make)
#   ./install.sh --uninstall  # remove installed files

set -euo pipefail

# ── ANSI colours (used in warnings) ─────────────────────────────────────────
RED='\033[0;31m'; YELLOW='\033[1;33m'; BOLD='\033[1m'; RESET='\033[0m'
if [[ ! -t 1 ]]; then RED=''; YELLOW=''; BOLD=''; RESET=''; fi

warn() { printf "${YELLOW}  WARNING: %s${RESET}\n" "$*" >&2; }
fatal(){ printf "${RED}${BOLD}  ERROR:   %s${RESET}\n" "$*" >&2; exit 1; }

# ── Platform detection ───────────────────────────────────────────────────────
UNAME_S="$(uname -s)"
case "${UNAME_S}" in
    Darwin)
        TSX_APP="/Applications/TheSkyX Professional Edition.app"
        PLUGIN_DIR="${TSX_APP}/Contents/PlugIns/RotatorPlugIns"
        LIST_DIR="${TSX_APP}/Contents/Resources/Common/Miscellaneous Files"
        LIB_NAME="libx2caarotator.dylib"
        HIDAPI_LIB="libhidapi"
        ;;
    Linux)
        TSX_HOME="${HOME}/TheSkyX"
        PLUGIN_DIR="${TSX_HOME}/Resources/Common/PlugIns64/RotatorPlugIns"
        LIST_DIR="${TSX_HOME}/Resources/Common/Miscellaneous Files"
        LIB_NAME="libx2caarotator.so"
        HIDAPI_LIB="libhidapi-hidraw"
        ;;
    *)
        fatal "Unsupported platform: ${UNAME_S}"
        ;;
esac

LIST_FILE="rotatorlist ZWO CAA.txt"
UI_FILE="x2caarotator.ui"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Runtime library check ────────────────────────────────────────────────────
check_runtime_libs() {
    local missing=0

    if [[ "${UNAME_S}" == "Linux" ]]; then
        # Use ldd against the built .so as the single authoritative check.
        # This avoids relying on ldconfig (not always in PATH) or shell glob
        # expansion (unreliable with set -o pipefail when some patterns don't match).
        if [[ -f "${SCRIPT_DIR}/${LIB_NAME}" ]]; then
            if ldd "${SCRIPT_DIR}/${LIB_NAME}" 2>/dev/null | grep -q "not found"; then
                local unresolved
                unresolved="$(ldd "${SCRIPT_DIR}/${LIB_NAME}" 2>/dev/null \
                              | grep "not found" | awk '{print $1}' | tr '\n' ' ')"
                # Specifically call out hidapi if it's the missing piece
                if echo "${unresolved}" | grep -qi hidapi; then
                    warn "hidapi runtime library not found (${unresolved% })."
                    printf "\n"
                    printf "${BOLD}  The plugin requires libhidapi (hidraw backend) to communicate\n"
                    printf "  with the ZWO CAA over USB HID.  Install it with:\n\n"
                    printf "    Debian/Ubuntu/Raspberry Pi OS:\n"
                    printf "      sudo apt install libhidapi-hidraw0\n\n"
                    printf "    Fedora/RHEL:\n"
                    printf "      sudo dnf install hidapi\n\n"
                    printf "    Arch Linux:\n"
                    printf "      sudo pacman -S hidapi\n${RESET}\n"
                else
                    warn "Unresolved shared libraries in ${LIB_NAME}: ${unresolved}"
                    printf "\n"
                    printf "${BOLD}  These libraries must be present on the target system at runtime.\n"
                    printf "  Install the relevant runtime packages for your distribution.${RESET}\n\n"
                fi
                missing=1
            fi
        fi

    elif [[ "${UNAME_S}" == "Darwin" ]]; then
        # Check for hidapi dylib via otool or by looking in Homebrew paths
        local found_hidapi=0
        for dir in /opt/homebrew/lib /usr/local/lib /usr/lib; do
            [[ -f "${dir}/libhidapi.dylib" ]] && { found_hidapi=1; break; }
        done
        if [[ $found_hidapi -eq 0 ]]; then
            # Try pkg-config as a secondary check
            pkg-config --exists hidapi 2>/dev/null && found_hidapi=1
        fi
        if [[ $found_hidapi -eq 0 ]]; then
            warn "hidapi runtime library not found."
            printf "\n"
            printf "${BOLD}  The plugin requires libhidapi to communicate with the ZWO CAA.\n"
            printf "  Install it with:\n\n"
            printf "    Homebrew (Intel or Apple Silicon):\n"
            printf "      brew install hidapi\n\n"
            printf "    MacPorts:\n"
            printf "      sudo port install hidapi${RESET}\n\n"
            missing=1
        fi

        # Verify dylib linkage
        if [[ -f "${SCRIPT_DIR}/${LIB_NAME}" ]]; then
            if otool -L "${SCRIPT_DIR}/${LIB_NAME}" 2>/dev/null | grep -q "not found\|missing"; then
                warn "Some linked libraries may be missing — check otool -L ${LIB_NAME}"
                missing=1
            fi
        fi
    fi

    return $missing
}

# ── Uninstall ────────────────────────────────────────────────────────────────
if [[ "${1:-}" == "--uninstall" ]]; then
    echo "Uninstalling ZWO CAA Rotator plugin..."
    rm -f "${PLUGIN_DIR}/${LIB_NAME}"      && echo "  Removed ${PLUGIN_DIR}/${LIB_NAME}"
    rm -f "${PLUGIN_DIR}/${UI_FILE}"       && echo "  Removed ${PLUGIN_DIR}/${UI_FILE}"
    rm -f "${LIST_DIR}/${LIST_FILE}"       && echo "  Removed ${LIST_DIR}/${LIST_FILE}"
    echo "Done."
    exit 0
fi

# ── Pre-flight checks ────────────────────────────────────────────────────────
if [[ ! -d "${PLUGIN_DIR}" ]]; then
    fatal "Plugin directory not found:\n  ${PLUGIN_DIR}\nIs TheSkyX installed?  Does RotatorPlugIns exist?"
fi

if [[ ! -d "${LIST_DIR}" ]]; then
    fatal "Miscellaneous Files directory not found:\n  ${LIST_DIR}"
fi

if [[ ! -f "${SCRIPT_DIR}/${LIB_NAME}" ]]; then
    fatal "${LIB_NAME} not found — run 'make clean && make' first."
fi

# Run library check; continue even if something is missing (warn, don't block)
echo "Checking runtime libraries..."
lib_ok=0
check_runtime_libs && lib_ok=1 || lib_ok=0

if [[ $lib_ok -eq 0 ]]; then
    printf "${YELLOW}${BOLD}  *** Installation will proceed, but the plugin may fail to load\n"
    printf "  *** in TheSkyX until the missing libraries are installed. ***${RESET}\n\n"
fi

# ── Install ──────────────────────────────────────────────────────────────────
echo "Installing plugin..."

cp "${SCRIPT_DIR}/${LIB_NAME}" "${PLUGIN_DIR}/${LIB_NAME}"
echo "  -> ${PLUGIN_DIR}/${LIB_NAME}"

cp "${SCRIPT_DIR}/${UI_FILE}" "${PLUGIN_DIR}/${UI_FILE}"
echo "  -> ${PLUGIN_DIR}/${UI_FILE}"

cp "${SCRIPT_DIR}/${LIST_FILE}" "${LIST_DIR}/${LIST_FILE}"
echo "  -> ${LIST_DIR}/${LIST_FILE}"

echo ""
echo "Installation complete."
if [[ $lib_ok -eq 0 ]]; then
    printf "${YELLOW}  Remember to install the missing runtime libraries above before\n"
    printf "  starting TheSkyX.${RESET}\n"
else
    echo "Restart TheSkyX and select 'ZWO CAA Rotator' from the Rotator device list."
fi

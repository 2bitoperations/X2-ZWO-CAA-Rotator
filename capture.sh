#!/usr/bin/env bash
# capture.sh — capture ZWO CAA USB HID protocol traffic via strace + usbmon
#
# Produces:
#   captures/TIMESTAMP/strace.txt   — every hidraw syscall with raw bytes
#   captures/TIMESTAMP/usbmon.pcap  — raw USB packets (if usbmon available)
#   captures/TIMESTAMP/run.log      — combined run log
#
# Usage:
#   bash capture.sh [target_angle]
#   target_angle passed through to test_caa (default 180.0)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMESTAMP="$(date '+%Y%m%d_%H%M%S')"
CAPDIR="${SCRIPT_DIR}/captures/${TIMESTAMP}"
LOG="${SCRIPT_DIR}/setup.log"

mkdir -p "${CAPDIR}"

exec > >(tee -a "${LOG}") 2>&1
echo ""
echo "========================================================"
echo "capture.sh  started  $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo "capture dir: ${CAPDIR}"
echo "========================================================"

TARGET_ANGLE="${1:-180.0}"

# ── 1. Verify test binary ─────────────────────────────────────────────────────
if [[ ! -x "${SCRIPT_DIR}/test_caa" ]]; then
    echo "ERROR: test_caa not found. Run setup.sh first."
    exit 1
fi

# ── 2. Identify hidraw node for the CAA (VID 03c3) ───────────────────────────
CAA_HIDRAW=""
for h in /dev/hidraw*; do
    if udevadm info "$h" 2>/dev/null | grep -qi "03[Cc]3"; then
        CAA_HIDRAW="$h"
        break
    fi
done

if [[ -z "${CAA_HIDRAW}" ]]; then
    echo "WARNING: Could not identify CAA hidraw node automatically."
    echo "         Strace will capture all hidraw nodes."
    HIDRAW_FILTER=""
else
    echo "==> CAA hidraw node: ${CAA_HIDRAW}"
    HIDRAW_FILTER="-e trace=openat,read,write,ioctl"
fi

# ── 3. Optional: load usbmon and start packet capture ────────────────────────
USB_BUS=$(lsusb | grep -i "03c3:1f20" | grep -oP 'Bus \K[0-9]+' | head -1)
USBMON_RUNNING=0

if [[ -n "${USB_BUS}" ]]; then
    echo "==> CAA on USB Bus ${USB_BUS}"
    USBMON_DEV="/dev/usbmon${USB_BUS}"

    if ! lsmod | grep -q usbmon; then
        echo "==> Loading usbmon kernel module..."
        sudo modprobe usbmon && echo "    usbmon loaded." || echo "    WARNING: could not load usbmon (continuing without it)"
    fi

    if [[ -e "${USBMON_DEV}" ]]; then
        echo "==> Starting usbmon capture on ${USBMON_DEV} -> ${CAPDIR}/usbmon.pcap"
        # tcpdump reads usbmon; tshark does too but may not be installed
        if command -v tcpdump &>/dev/null; then
            sudo tcpdump -i "usbmon${USB_BUS}" -w "${CAPDIR}/usbmon.pcap" &
            TCPDUMP_PID=$!
            USBMON_RUNNING=1
            echo "    tcpdump PID ${TCPDUMP_PID}"
        elif command -v tshark &>/dev/null; then
            tshark -i "usbmon${USB_BUS}" -w "${CAPDIR}/usbmon.pcap" &
            TCPDUMP_PID=$!
            USBMON_RUNNING=1
            echo "    tshark PID ${TCPDUMP_PID}"
        else
            echo "    WARNING: neither tcpdump nor tshark found; skipping pcap capture."
            echo "    Install with: sudo apt-get install -y tcpdump"
        fi
    else
        echo "    WARNING: ${USBMON_DEV} not found even after modprobe; skipping pcap."
    fi
else
    echo "WARNING: Could not determine USB bus for CAA. Skipping usbmon capture."
fi

# ── 4. strace run ─────────────────────────────────────────────────────────────
# Capture: open/openat (to see which fds map to which device nodes)
#          read / write (HID output/input reports)
#          ioctl        (HIDIOCGFEATURE / HIDIOCSFEATURE feature reports)
#          close        (for fd lifecycle)
# -x   : print all strings as hex
# -e read=all, write=all : dump full buffer contents for every read/write
# -s 256 : print up to 256 bytes of string args
# -ttt  : microsecond timestamps (Unix epoch)
# -ff   : follow forks into separate files (library may spawn threads)
#
# Note: strace output goes to strace.txt; test_caa stdout/stderr to run.log
echo ""
echo "==> Running: strace ./test_caa ${TARGET_ANGLE}"
echo "    strace output -> ${CAPDIR}/strace.txt"
echo ""

strace \
    -o "${CAPDIR}/strace.txt" \
    -e trace=openat,read,write,ioctl,close \
    -e read=all \
    -e write=all \
    -x -s 256 -ttt -ff \
    "${SCRIPT_DIR}/test_caa" "${TARGET_ANGLE}" \
    2>&1 | tee "${CAPDIR}/run.log"

echo ""
echo "==> test_caa exited."

# ── 5. Stop usbmon capture ────────────────────────────────────────────────────
if [[ "${USBMON_RUNNING}" -eq 1 ]]; then
    echo "==> Stopping packet capture (PID ${TCPDUMP_PID})..."
    sudo kill "${TCPDUMP_PID}" 2>/dev/null || true
    wait "${TCPDUMP_PID}" 2>/dev/null || true
    echo "    Saved: ${CAPDIR}/usbmon.pcap"
fi

# ── 6. Filter strace output to just hidraw traffic ───────────────────────────
echo "==> Extracting hidraw traffic from strace..."

# Find the fd number(s) used for the CAA hidraw node
STRACE_MAIN="${CAPDIR}/strace.txt"

if [[ -f "${STRACE_MAIN}" ]]; then
    # Extract lines that open a hidraw device, plus subsequent reads/writes/ioctls on those fds
    python3 - "${STRACE_MAIN}" "${CAPDIR}/hidraw_traffic.txt" << 'PYEOF'
import sys, re

in_file  = sys.argv[1]
out_file = sys.argv[2]

hidraw_fds = set()
lines = open(in_file).readlines()
out = []

for line in lines:
    # Track opens: openat(... "hidraw...", ...) = FD
    m = re.search(r'openat\(.*"(/dev/hidraw[^"]+)".*=\s*(\d+)', line)
    if m:
        hidraw_fds.add(m.group(2))
        out.append(f"# OPEN  fd={m.group(2)}  path={m.group(1)}\n")
        out.append(line)
        continue

    # Capture read/write/ioctl on a known hidraw fd
    m2 = re.match(r'\d+\.\d+\s+(\w+)\((\d+)', line)
    if m2 and m2.group(2) in hidraw_fds:
        out.append(line)
        continue

    # Capture close of a known hidraw fd
    m3 = re.search(r'close\((\d+)\)', line)
    if m3 and m3.group(1) in hidraw_fds:
        out.append(line)
        hidraw_fds.discard(m3.group(1))

with open(out_file, 'w') as f:
    f.writelines(out)
print(f"  Filtered {len(out)} lines -> {out_file}")
PYEOF
else
    echo "    WARNING: strace.txt not found (strace may have written per-thread files)."
    echo "    Per-thread files: $(ls ${CAPDIR}/strace.txt.* 2>/dev/null | wc -l) found."
    echo "    Re-run without -ff or cat them manually."
fi

echo ""
echo "========================================================"
echo "Capture complete: ${CAPDIR}/"
ls -lh "${CAPDIR}/"
echo "========================================================"
echo "Next step: review captures/${TIMESTAMP}/hidraw_traffic.txt"

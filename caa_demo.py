#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
ZWO CAA rotator demo — pure Python, ZERO ZWO library dependency.

Implements the HID protocol reverse-engineered from libCAARotator v1.5.9.
Communicates directly with /dev/hidrawN via HIDIOCSFEATURE / HIDIOCGFEATURE
ioctls — no ZWO binary, no INDI, no libhidapi Python binding required.

Usage:
    uv run caa_demo.py              # full demo, 5° test sweep
    uv run caa_demo.py --angle 10   # custom sweep angle
    uv run caa_demo.py --info-only  # just print device info and exit
"""

from __future__ import annotations

import argparse
import fcntl
import os
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path


# ── USB identity ──────────────────────────────────────────────────────────────
VID = 0x03C3
PID = 0x1F20

# ── HID ioctl numbers ─────────────────────────────────────────────────────────
# _IOC(dir, type, nr, size) — Linux generic macro, works on all architectures.
_IOC_WRITE = 1
_IOC_READ  = 2

def _ioc(direction: int, type_: str, nr: int, size: int) -> int:
    return (direction << 30) | (size << 16) | (ord(type_) << 8) | nr

def HIDIOCSFEATURE(n: int) -> int:   # host → device feature report
    return _ioc(_IOC_WRITE | _IOC_READ, 'H', 0x06, n)

def HIDIOCGFEATURE(n: int) -> int:   # device → host feature report
    return _ioc(_IOC_WRITE | _IOC_READ, 'H', 0x07, n)


# ── Protocol constants ────────────────────────────────────────────────────────
# Frame header bytes (bytes 1-2 of every packet)
MAGIC = (0x7E, 0x5A)

# Output report ID (host → device)
REPORT_OUT = 0x03
# Input report ID (device → host)
REPORT_IN  = 0x01

# CMD byte (byte 3)
CMD_QUERY       = 0x02   # read a register
CMD_MOVE        = 0x03   # move / config
CMD_SET_BEEP    = 0x07   # write beep flag
CMD_SET_REVERSE = 0x09   # write reverse flag

# CMD_QUERY sub-registers (byte 4)
REG_STATE   = 0x03   # is_moving + position
REG_STATUS2 = 0x08   # beep + reverse + position
REG_INFO    = 0x04   # firmware version + type string
REG_SERIAL  = 0x0C   # 8-byte serial number

# CMD_MOVE sub-types (byte 4)
MOVE_ABSOLUTE = 0x01   # move to absolute angle
MOVE_STOP     = 0x02   # stop motion (all zeros after sub-type)
MOVE_CONFIG   = 0x00   # set config / max-degree (used by SetMaxDegree)


# ── Data classes ──────────────────────────────────────────────────────────────
@dataclass
class State:
    is_moving: bool
    angle: float       # degrees
    max_angle: float   # degrees

@dataclass
class DeviceInfo:
    firmware: str      # e.g. "1.1.1"
    type_str: str      # e.g. "CAA-M54"
    serial: str        # e.g. "060030EDA0CBFA7C"
    beep: bool
    reverse: bool
    max_angle: float


# ── Device driver ─────────────────────────────────────────────────────────────
class CAADevice:
    """
    Direct HID driver for the ZWO CAA rotator.

    Protocol summary
    ────────────────
    Every exchange is a 16-byte HIDIOCSFEATURE followed (for read commands)
    by a 17-byte HIDIOCGFEATURE.  All packets begin:
        [REPORT_ID] [0x7E] [0x5A] [CMD] [payload …]

    Position encoding: 32-bit big-endian unsigned int = angle × 10000
    """

    def __init__(self, path: str) -> None:
        self._path = path
        self._fd   = os.open(path, os.O_RDWR)
        self._max_angle: float = 360.0   # updated on every state read

    def close(self) -> None:
        os.close(self._fd)

    def __enter__(self) -> CAADevice:
        return self

    def __exit__(self, *_) -> None:
        self.close()

    # ── low-level I/O ─────────────────────────────────────────────────────────

    def _send(self, payload: bytes) -> None:
        """Send a 16-byte feature report.  payload starts with CMD byte."""
        buf = bytearray(16)
        buf[0] = REPORT_OUT
        buf[1] = MAGIC[0]
        buf[2] = MAGIC[1]
        n = min(len(payload), 13)
        buf[3:3 + n] = payload[:n]
        fcntl.ioctl(self._fd, HIDIOCSFEATURE(16), buf)

    def _query(self, payload: bytes) -> bytes:
        """Send a query and return the 17-byte response.

        The device echoes the register byte in buf[3].  Some registers
        (GET_INFO, GET_SERIAL) have ~200 ms processing latency; if the
        echo byte doesn't match we retry HIDIOCGFEATURE rather than
        returning a stale response from a previous command.
        """
        self._send(payload)
        expected_echo = payload[1]   # register byte echoed in response[3]
        buf = bytearray(17)
        buf[0] = REPORT_IN
        for _ in range(8):
            fcntl.ioctl(self._fd, HIDIOCGFEATURE(17), buf)
            if buf[3] == expected_echo:
                return bytes(buf)
            time.sleep(0.05)
        return bytes(buf)

    # ── encoding helpers ──────────────────────────────────────────────────────

    @staticmethod
    def _enc_angle(degrees: float) -> bytes:
        """Encode angle to 4-byte big-endian (units: 1/10000 degree)."""
        return struct.pack('>I', round(degrees * 10000))

    @staticmethod
    def _dec_angle(data: bytes, offset: int) -> float:
        """Decode 4-byte big-endian angle from a response buffer."""
        return struct.unpack_from('>I', data, offset)[0] / 10000.0

    # ── read commands ─────────────────────────────────────────────────────────

    def get_state(self) -> State:
        """CMD 0x02 / reg 0x03 — current position and motion flag."""
        r = self._query(bytes([CMD_QUERY, REG_STATE]))
        is_moving = bool(r[4])
        angle     = self._dec_angle(r, 6)
        max_angle = float(struct.unpack_from('>H', r, 13)[0])
        self._max_angle = max_angle
        return State(is_moving=is_moving, angle=angle, max_angle=max_angle)

    def get_status2(self) -> dict:
        """CMD 0x02 / reg 0x08 — beep, reverse, position."""
        r = self._query(bytes([CMD_QUERY, REG_STATUS2]))
        return {
            'beep':      bool(r[4]),
            'reverse':   bool(r[5]),
            'angle':     self._dec_angle(r, 6),
            'max_angle': float(struct.unpack_from('>H', r, 13)[0]),
        }

    def get_info(self) -> tuple[str, str]:
        """CMD 0x02 / reg 0x04 — firmware version string and type string."""
        r = self._query(bytes([CMD_QUERY, REG_INFO]))
        firmware = f"{r[4]}.{r[5]}.{r[6]}"
        # byte[7] is a device-class byte ('E' = 0x45); type string starts at [8]
        type_str = r[8:16].rstrip(b'\x00').decode('ascii', errors='replace')
        return firmware, type_str

    def get_serial(self) -> str:
        """CMD 0x02 / reg 0x0C — 8-byte serial number as hex string."""
        r = self._query(bytes([CMD_QUERY, REG_SERIAL]))
        return r[4:12].hex().upper()

    def get_device_info(self) -> DeviceInfo:
        """Composite: gather all read-only device info.

        Call GET_STATE first to flush any stale data from the device's
        response buffer.  GET_INFO and GET_SERIAL are slow registers; the
        device updates byte[3] (echo) immediately but leaves bytes[6-16]
        from the previous response until a GET_STATE completes.  The ZWO
        library uses the same STATE→STATUS2→INFO open sequence for this
        reason.
        """
        self.get_state()
        s2                 = self.get_status2()
        firmware, type_str = self.get_info()
        serial             = self.get_serial()
        return DeviceInfo(
            firmware=firmware,
            type_str=type_str,
            serial=serial,
            beep=s2['beep'],
            reverse=s2['reverse'],
            max_angle=s2['max_angle'],
        )

    # ── write commands ────────────────────────────────────────────────────────

    def move_to(self, angle: float) -> None:
        """
        CMD 0x03 / sub 0x01 — MOVE_ABSOLUTE.
        CAAMove (relative) and CAAMoveToMechanical use this same opcode;
        the library converts them to absolute before sending.
        No response — poll get_state() to track completion.
        """
        payload = bytes([CMD_MOVE, MOVE_ABSOLUTE, 0x00]) \
                + self._enc_angle(angle) \
                + bytes([0x00, 0x00, 0x00, 0x00]) \
                + struct.pack('>H', int(self._max_angle))
        self._send(payload)

    def stop(self) -> None:
        """CMD 0x03 / sub 0x02 (zeros) — STOP.  No response."""
        self._send(bytes([CMD_MOVE, MOVE_STOP]))

    def set_beep(self, enabled: bool) -> None:
        """CMD 0x07 — SET_BEEP.  No response."""
        self._send(bytes([CMD_SET_BEEP, int(enabled)]))

    def set_reverse(self, enabled: bool) -> None:
        """CMD 0x09 — SET_REVERSE.  No response."""
        self._send(bytes([CMD_SET_REVERSE, int(enabled)]))

    def set_max_degree(self, degrees: float) -> None:
        """
        CMD 0x03 / sub 0x00 — SET_CONFIG.
        bytes[6-7] = 0x0BB8 (3000) — constant observed in all captures
                     (believed to be a speed/rate parameter)
        bytes[10]  = 0x02          — constant; purpose unknown
        bytes[14-15] = max_angle as big-endian uint16
        """
        payload = bytes([
            CMD_MOVE, MOVE_CONFIG,
            0x00,
            0x0B, 0xB8,       # constant speed/rate param
            0x00, 0x00,
            0x02,             # constant mode param
            0x00, 0x00, 0x00,
        ]) + struct.pack('>H', int(degrees))
        self._send(payload)
        self._max_angle = degrees

    # ── convenience ───────────────────────────────────────────────────────────

    def wait_for_stop(self, timeout: float = 120.0, poll_interval: float = 0.2) -> float:
        """Poll get_state() until is_moving is False.  Returns final angle."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.get_state()
            if not state.is_moving:
                return state.angle
            print('.', end='', flush=True)
            time.sleep(poll_interval)
        raise TimeoutError(f"Motion did not stop within {timeout}s")

    def move_and_wait(self, angle: float) -> float:
        """move_to() then wait_for_stop(); returns final position."""
        self.move_to(angle)
        pos = self.wait_for_stop()
        print()
        return pos


# ── Device discovery ──────────────────────────────────────────────────────────

def find_caa_hidraw() -> str:
    """
    Walk /sys/class/hidraw/ and return the /dev/hidrawN path whose uevent
    matches VID=03C3 PID=1F20.
    """
    hidraw_root = Path('/sys/class/hidraw')
    if not hidraw_root.exists():
        raise RuntimeError("/sys/class/hidraw not found — is this Linux?")

    for node in sorted(hidraw_root.iterdir()):
        uevent = node / 'device' / 'uevent'
        try:
            for line in uevent.read_text().splitlines():
                if line.startswith('HID_ID='):
                    # Format: HID_ID=0003:000003C3:00001F20
                    parts = line.split('=', 1)[1].split(':')
                    if (len(parts) == 3
                            and int(parts[1], 16) == VID
                            and int(parts[2], 16) == PID):
                        return f'/dev/{node.name}'
        except (OSError, ValueError):
            continue

    raise RuntimeError(
        f"ZWO CAA not found (VID={VID:04X} PID={PID:04X}).\n"
        "Check: lsusb | grep 03c3, udev rules installed, device plugged in."
    )


# ── Demo ──────────────────────────────────────────────────────────────────────

def demo(path: str, sweep: float, info_only: bool) -> None:
    print(f"Opening {path}")

    with CAADevice(path) as dev:

        # ── Device info ───────────────────────────────────────────────────────
        print("\n── Device info ──")
        info = dev.get_device_info()
        print(f"  Type:      {info.type_str}")
        print(f"  Firmware:  {info.firmware}")
        print(f"  Serial:    {info.serial}")
        print(f"  Beep:      {'on' if info.beep else 'off'}")
        print(f"  Reverse:   {'yes' if info.reverse else 'no'}")
        print(f"  Max angle: {info.max_angle:.0f}°")

        state = dev.get_state()
        print(f"\n── Current state ──")
        print(f"  Position:  {state.angle:.4f}°")
        print(f"  Moving:    {state.is_moving}")
        start_pos = state.angle

        if info_only:
            return

        # ── Beep toggle ───────────────────────────────────────────────────────
        print(f"\n── Beep toggle ──")
        print(f"  Sending SET_BEEP off (CMD=0x07, byte[4]=0x00)")
        dev.set_beep(False)
        s2 = dev.get_status2()
        print(f"  STATUS2 beep = {s2['beep']}  (expected False)")

        print(f"  Sending SET_BEEP on  (CMD=0x07, byte[4]=0x01)")
        dev.set_beep(True)
        s2 = dev.get_status2()
        print(f"  STATUS2 beep = {s2['beep']}  (expected True)")

        # ── Reverse toggle ────────────────────────────────────────────────────
        print(f"\n── Reverse toggle ──")
        print(f"  Sending SET_REVERSE true  (CMD=0x09, byte[4]=0x01)")
        dev.set_reverse(True)
        s2 = dev.get_status2()
        print(f"  STATUS2 reverse = {s2['reverse']}  (expected True)")

        print(f"  Sending SET_REVERSE false (CMD=0x09, byte[4]=0x00)")
        dev.set_reverse(False)
        s2 = dev.get_status2()
        print(f"  STATUS2 reverse = {s2['reverse']}  (expected False)")

        # ── SetMaxDegree ──────────────────────────────────────────────────────
        orig_max = state.max_angle
        print(f"\n── SetMaxDegree ──")
        print(f"  Current max = {orig_max:.0f}°")
        test_max = 180.0 if orig_max > 180.0 else orig_max
        print(f"  Sending SET_CONFIG max={test_max:.0f}°  (CMD=0x03, sub=0x00, bytes[14-15]=0x{int(test_max):04X})")
        dev.set_max_degree(test_max)
        s = dev.get_state()
        print(f"  GET_STATE max_angle = {s.max_angle:.0f}°  (expected {test_max:.0f})")

        print(f"  Restoring max={orig_max:.0f}°")
        dev.set_max_degree(orig_max)
        s = dev.get_state()
        print(f"  GET_STATE max_angle = {s.max_angle:.0f}°  (restored)")

        # ── Move ──────────────────────────────────────────────────────────────
        target = round(start_pos + sweep, 2)
        print(f"\n── MOVE_ABSOLUTE: {start_pos:.4f}° → {target:.4f}° ──")
        print(f"  Sending CMD=0x03 sub=0x01  angle_u32=0x{round(target*10000):08X}  ({round(target*10000)})")
        print(f"  Polling GET_STATE (CMD=0x02 reg=0x03) every 200 ms ", end='', flush=True)
        final = dev.move_and_wait(target)
        print(f"  Arrived at {final:.4f}°  (error {abs(final-target)*10000:.0f} units = {abs(final-target)*1000:.1f} mdeg)")

        # ── Stop mid-move ─────────────────────────────────────────────────────
        stop_target = round(final + sweep, 2)
        print(f"\n── STOP mid-move ──")
        print(f"  Starting MOVE_ABSOLUTE to {stop_target:.4f}°")
        dev.move_to(stop_target)
        time.sleep(0.3)
        print(f"  Sending STOP (CMD=0x03, sub=0x02, all zeros)")
        dev.stop()
        stopped_at = dev.wait_for_stop()
        print()
        print(f"  Stopped at {stopped_at:.4f}°  (moved {abs(stopped_at-final):.4f}° before stop)")

        # ── Return to start ───────────────────────────────────────────────────
        print(f"\n── Return to start ──")
        print(f"  MOVE_ABSOLUTE to {start_pos:.4f}° ", end='', flush=True)
        restored = dev.move_and_wait(start_pos)
        print(f"  Back at {restored:.4f}°")

        # ── Summary ───────────────────────────────────────────────────────────
        print("\n── Summary ──")
        print(f"  All commands sent and acknowledged over raw HID feature reports.")
        print(f"  Zero ZWO library code used.")
        print()
        print("  Commands exercised:")
        print("    CMD 0x02 reg 0x03  GET_STATE         — position + is_moving")
        print("    CMD 0x02 reg 0x08  GET_STATUS2       — beep + reverse + position")
        print("    CMD 0x02 reg 0x04  GET_INFO          — firmware + type")
        print("    CMD 0x02 reg 0x0C  GET_SERIAL        — serial number")
        print("    CMD 0x03 sub 0x01  MOVE_ABSOLUTE     — move to angle")
        print("    CMD 0x03 sub 0x02  STOP              — halt motion")
        print("    CMD 0x03 sub 0x00  SET_CONFIG        — set max degree")
        print("    CMD 0x07           SET_BEEP          — beep on/off")
        print("    CMD 0x09           SET_REVERSE       — reverse on/off")


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="ZWO CAA rotator demo — pure Python, no ZWO library"
    )
    parser.add_argument(
        '--angle', type=float, default=5.0,
        help='Sweep angle in degrees for move tests (default: 5.0)',
    )
    parser.add_argument(
        '--device', type=str, default=None,
        help='HID device path, e.g. /dev/hidraw3 (auto-detect if omitted)',
    )
    parser.add_argument(
        '--info-only', action='store_true',
        help='Print device info and exit without moving',
    )
    args = parser.parse_args()

    path = args.device or find_caa_hidraw()

    try:
        demo(path, sweep=args.angle, info_only=args.info_only)
    except PermissionError:
        print(f"ERROR: no permission to open {path}", file=sys.stderr)
        print("Install the udev rule:  sudo install -m644 "
              "~/appinstall/indi-3rdparty/libasi/99-asi.rules "
              "/etc/udev/rules.d/99-asi.rules && sudo udevadm control --reload-rules",
              file=sys.stderr)
        sys.exit(1)
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()

# Agent Guide

This is a TheSkyX X2 Rotator plugin for the ZWO CAA Camera Angle Adjuster.
See [PROTOCOL.md](PROTOCOL.md) for the USB HID wire protocol.

## Build & install

**Always `make clean && make install`** — never cmake. Build must produce zero
warnings. The `UI OK.` line from the Makefile confirms the .ui file passed
XML validation.

**Runtime dependency:** hidapi (hidraw backend on Linux).
- Linux: `sudo apt install libhidapi-hidraw0` (or `libhidapi-dev` for builds)
- macOS: `brew install hidapi`
- Windows (MSYS2): `pacman -S mingw-w64-x86_64-hidapi`

`install.sh` checks for the runtime library and prints OS-specific install
instructions if it is missing.

## Architecture

Two-layer design:

| Layer | Files | Purpose |
|---|---|---|
| Low-level HID | `caarotator.h/cpp` | Direct `/dev/hidrawN` I/O; no X2 deps |
| X2 plugin | `x2caarotator.h/cpp` | TheSkyX interfaces; uses `CAARotator` |

**`CAARotator`** — cross-platform HID driver (hidapi):
- `enumerateDevices()` uses `hid_enumerate(VID, PID)` and briefly opens each device to read serial/firmware/type
- `open(path)` / `close()` — `hid_open_path()` / `hid_close()`
- `getState(s)` — calls GET_STATE then GET_STATUS2 (ZWO library open sequence: state flush first to avoid stale STATUS2 data)
- `moveTo(deg)` / `stop()` / `setBeep()` / `setReverse()` / `setMaxDegree()`
- On non-Linux platforms, all methods return `CAART_ERR_UNSUPPORTED`

**`X2CAARotator`** implements:

| Interface | Purpose |
|---|---|
| `RotatorDriverInterface` | Core API: `position`, `startRotatorGoto`, `isCompleteRotatorGoto`, `endRotatorGoto`, `abort` |
| `ModalSettingsDialogInterface` | Settings dialog: `execModalSettingsDialog` |
| `X2GUIEventInterface` | UI callbacks: `uiEvent` |

Goto state machine: `startRotatorGoto` → `moveTo`; `isCompleteRotatorGoto` polls
`getState().isMoving`; `endRotatorGoto` clears `m_bDoingGoto`.

**Device selection** (`chooseDevice()`):
1. Match `DeviceSerial` against `CAADeviceEntry::serial` (preferred — stable across USB reconnects)
2. Match `DevicePath` against `CAADeviceEntry::path` (hint — stale after replug)
3. First available device (first-time setup / single-device case)

Settings persistence: `BasicIniUtilInterface`, section `"CAARotator"`, keys
`DeviceSerial`, `DevicePath`, `Beep`, `Reverse`, `MaxAngle`, `DebugLevel`.

## Code rules

- **C++11** only (`-std=gnu++11`). No C++14/17. No exceptions. No RTTI except
  `dynamic_cast` in `queryAbstraction`.
- Error returns: `SB_OK` / `ERR_*` from `sberrorx.h`. Never throw.
- Logging: `logDebug(minLevel, fmt, ...)`. Level 1=Errors, 2=Cmds, 3=Full I/O.
- Do not add new base classes without updating `queryAbstraction`.
- `licensedinterfaces/` is vendored (Software Bisque copyright). The
  `X2_FLAT_INCLUDES` patch in `x2guiinterface.h` is load-bearing — do not
  upgrade headers without re-verifying it.
- No `SerialPortParams2Interface` — this device is HID, not serial.

## HID protocol summary

All communication via HID feature reports on `/dev/hidrawN`:
- Output: 16 bytes `HIDIOCSFEATURE(16)` — `[0x03][0x7E][0x5A][CMD][params]`
- Input:  17 bytes `HIDIOCGFEATURE(17)` — `[0x01][0x7E][0x5A][echo][data]`

Key registers (CMD 0x02 QUERY):
- `0x03` GET_STATE: is_moving, angle (BE32 ×10000°), max_angle (BE16 integer°)
- `0x08` GET_STATUS2: beep, reverse, angle, max_angle
- `0x04` GET_INFO: firmware version bytes, type string at byte[8]
- `0x0C` GET_SERIAL: 8-byte serial number

**Response-buffer timing**: GET_INFO and GET_SERIAL are slow registers — the
device updates byte[3] (echo) quickly but leaves bytes[6-16] stale. Always call
GET_STATE before GET_STATUS2 to flush. `CAARotator::getState()` does this
automatically; `query()` retries HIDIOCGFEATURE until the echo matches.

## UI file rules

Widget naming: `lbl` `spin` `combo` `btn` `grp` `chk` prefixes.

**`pushButtonOK` and `pushButtonCancel` must keep those exact names.** The X2
framework hardcodes them to wire up accept/reject.

**No `QScrollArea`.** Crashes TheSkyX on OK click during dialog teardown.

**Dialog geometry is ignored.** Size is controlled by TheSkyX internally.

## What didn't work

| Approach | Why |
|---|---|
| `QScrollArea` | TSX crashes on OK — X2GUI teardown can't handle the extra container |
| Renaming `pushButtonOK`/`pushButtonCancel` | X2 hardcodes these names |
| Reading GET_STATUS2 before GET_STATE on fresh open | Stale bytes[6-16] from previous slow register causes wrong max_angle/beep/reverse |
| Relying on HIDIOCGFEATURE echo retry alone to fix stale data | Echo matches quickly but data bytes lag — need explicit GET_STATE flush |

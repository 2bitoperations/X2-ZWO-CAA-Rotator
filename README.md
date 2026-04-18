# ZWO CAA Rotator — Protocol Reverse Engineering

## Goal

Produce a fully open-source driver for the ZWO CAA (Camera Angle Adjuster) rotator that has
**zero dependency on any ZWO closed-source binary**.

The ZWO CAA communicates over USB HID (VID `0x03c3`, PID `0x1f20`).  ZWO ships a prebuilt shared
library (`libCAARotator`) that wraps the raw HID protocol.  The INDI project uses that library in
`indi-3rdparty/indi-asi/asi_rotator.cpp`.  Our path is:

1. **Phase 1 (this repo)** — build and validate the INDI + ZWO-library stack so we have a working
   baseline and can observe every API call the library makes over USB.
2. **Phase 2** — capture USB traffic (via `usbmon` / Wireshark) while exercising each
   `CAA_API` function; document the raw HID report format.
3. **Phase 3** — write a replacement library / driver that speaks the raw HID protocol directly,
   validated against the Phase 1 baseline.

---

## Hardware

| Field | Value |
|---|---|
| Device | ZWO CAA rotator |
| USB ID | `03c3:1f20` ZWO Device |
| Interface | USB HID |

---

## Repository layout

```
X2-ZWO-CAA-Rotator/
├── README.md           ← this file
├── setup.sh            ← installs deps, installs the ZWO library, builds the test harness
└── test_caa.cpp        ← standalone test: connect → read position → move → read position
```

The INDI source lives at `~/appinstall/indi-3rdparty/` (cloned separately).

---

## Phase 1: build and test with ZWO library

### 1.1  Install build dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake build-essential pkg-config \
    libindi-dev \
    libusb-1.0-0-dev \
    libhidapi-dev \
    libudev-dev
```

> **Note:** `libindi-dev` pulls in the INDI framework headers and libraries required to compile
> `indi_asi_rotator`.  On Ubuntu/Debian you can also build INDI from source — see
> `~/appinstall/indi-3rdparty/../` for the core `indi` repo if needed.

### 1.2  Install the ZWO CAA shared library

The prebuilt aarch64 binary is already present inside the `indi-3rdparty` checkout:

```
~/appinstall/indi-3rdparty/libasi/armv8/libCAARotator.bin
```

It is a standard ELF shared library with a `.bin` extension.  Install it system-wide:

```bash
sudo install -m 644 \
    ~/appinstall/indi-3rdparty/libasi/armv8/libCAARotator.bin \
    /usr/local/lib/libCAARotator.so.1

sudo ln -sf /usr/local/lib/libCAARotator.so.1 /usr/local/lib/libCAARotator.so
sudo ldconfig
```

Install the public header:

```bash
sudo install -m 644 \
    ~/appinstall/indi-3rdparty/libasi/CAA_API.h \
    /usr/local/include/CAA_API.h
```

Install the udev rule so the device is accessible without root:

```bash
sudo install -m 644 \
    ~/appinstall/indi-3rdparty/libasi/99-asi.rules \
    /etc/udev/rules.d/99-asi.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Or run `setup.sh` which does all of the above.

### 1.3  Build the test harness

```bash
cd ~/appinstall/X2-ZWO-CAA-Rotator
g++ -std=c++17 -Wall \
    -I/usr/local/include \
    -o test_caa test_caa.cpp \
    -L/usr/local/lib -lCAARotator \
    -lusb-1.0 -lpthread
```

Or run `setup.sh` which builds it automatically.

### 1.4  Run the test harness

Plug in the CAA, then:

```bash
./test_caa
```

Expected output (success):

```
SDK version: 1, 5, 9
Found 1 CAA rotator(s)
  [0] ID=0  Name="ZWO CAA"  MaxStep=360
Opening ID 0 ...  OK
Firmware: 1.0.0
Serial:   XXXXXXXXXXXXXXXX
Current position: 123.45 deg
Moving to 180.0 deg ...
Waiting for motion to complete ...
Position after move: 180.00 deg
Moving back to original position (123.45 deg) ...
Waiting for motion to complete ...
Position restored: 123.45 deg
Closed.
```

If you see `Found 0 CAA rotator(s)` check:
- The udev rule is installed and the device node is accessible.
- `lsusb` shows `03c3:1f20`.
- `ldconfig -p | grep CAA` shows the library.

---

## Phase 2: USB traffic capture (planned)

Once Phase 1 passes we will:

1. Load the `usbmon` kernel module: `sudo modprobe usbmon`
2. Identify the bus number from `lsusb` (e.g. Bus 001).
3. Capture with Wireshark or `tshark -i usbmon1 -w caa_capture.pcapng`.
4. Run `test_caa` while capturing.
5. Decode HID reports, correlating each `CAA_API` call to its USB frames.

Decoded report structures will be documented in `PROTOCOL.md` (to be created).

---

## Phase 3: open-source driver (planned)

Target: a `libcaa_open.so` presenting the same `CAA_API.h` interface but implemented entirely in
C over raw `libhidapi-libusb`, with no ZWO binary dependency.  Integration target is both INDI
(`indi_asi_rotator` drop-in) and a future TheSkyX X2 plugin.

---

## References

- `~/appinstall/indi-3rdparty/indi-asi/asi_rotator.cpp` — INDI rotator driver source
- `~/appinstall/indi-3rdparty/libasi/CAA_API.h` — ZWO CAA SDK public API (fully documented)
- `~/appinstall/indi-3rdparty/libasi/armv8/libCAARotator.bin` — prebuilt aarch64 library
- `~/appinstall/indi-3rdparty/libasi/99-asi.rules` — udev rules
- INDI 3rd-party repo: https://github.com/indilib/indi-3rdparty

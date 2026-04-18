# ZWO CAA Rotator — USB HID Protocol

Reverse engineered from `libCAARotator` v1.5.9 via `LD_PRELOAD` ioctl interception.
Captures: `captures/intercept_5deg_20260417_173021.log` and
`captures/intercept_probe_20260417_174147.log`.
Device: CAA-M54, firmware 1.1.1, serial `060030EDA0CBFA7C`, aarch64.

---

## Transport

| Property | Value |
|---|---|
| USB VID/PID | `0x03C3` / `0x1F20` |
| Interface | USB HID |
| Access | `/dev/hidraw*` via `HIDIOCSFEATURE` / `HIDIOCGFEATURE` ioctls |
| Output report ID | `0x03` (host → device), 16 bytes total |
| Input report ID | `0x01` (device → host), 17 bytes total |

All communication is via HID **feature reports** only. No interrupt/bulk transfers.
Every exchange is one `HIDIOCSFEATURE(16)` (send) optionally followed by one
`HIDIOCGFEATURE(17)` (receive). Move and write commands have no response.

The library enumerates devices by walking `/sys/class/hidraw/` and comparing VID
via udev; it opens the device with `O_RDWR`.

---

## Frame format

### Request (host → device): 16 bytes

```
Offset  Size  Field
------  ----  -----
0       1     Report ID = 0x03
1       1     Magic = 0x7E
2       1     Magic = 0x5A
3       1     CMD
4       12    Parameters (command-specific, zero-padded)
```

### Response (device → host): 17 bytes

```
Offset  Size  Field
------  ----  -----
0       1     Report ID = 0x01
1       1     Magic = 0x7E
2       1     Magic = 0x5A
3       1     CMD or register echo
4       13    Response data (command-specific)
```

---

## Position encoding

All positions (in both commands and responses) are encoded as a **32-bit
big-endian unsigned integer = angle_degrees × 10000**.

```
angle_u32 = round(angle_degrees * 10000)
angle_degrees = angle_u32 / 10000.0
```

Examples: 4.90° = `0x0000BF68` (49000), 9.89° = `0x00018254` (98900).

Resolution: 0.0001° (1/10000 degree).

---

## Command reference

### CMD 0x02 — QUERY (read register)

Byte[4] selects the register. Response echoes the register in byte[3].

```
Request:  03 7E 5A 02 [REG] 00 00 00 00 00 00 00 00 00 00 00
Response: 01 7E 5A [REG] [data ...]
```

---

#### Register 0x03 — GET_STATE

Returns motion status and current position. Polled every ~200 ms during moves.

```
Response byte  Field
─────────────  ─────────────────────────────────────────────
[3]            0x03 (echo)
[4]            is_moving  (0 = idle, 1 = moving)
[5]            0x00 (reserved / unobserved)
[6..9]         current_angle_u32  (BE32, ×10000°)
[10..12]       0x00 (reserved / unobserved)
[13..14]       max_angle_u16  (BE16, integer degrees; e.g. 0x0168 = 360)
[15..16]       0x00
```

Live samples (CAA-M54 moving from 4.90° toward 9.90°):

| is_moving | angle_u32  | degrees  |
|-----------|------------|---------|
| 0         | 0x0000BF68 | 4.9000° |
| 1         | 0x0000CF6B | 5.3099° |
| 1         | 0x00010BF8 | 6.8600° |
| 1         | 0x00014758 | 8.3800° |
| 1         | 0x00016F30 | 9.4000° |
| 0         | 0x00018254 | 9.8900° |

---

#### Register 0x08 — GET_STATUS2

Returns device configuration flags and position. Polled after SET_BEEP and
SET_REVERSE to verify the write was accepted.

**Response-buffer timing note**: GET_INFO (reg 0x04) and GET_SERIAL (reg 0x0C)
are slow registers — the device updates byte[3] (echo) quickly but leaves
bytes[6-16] of the response buffer populated with the previous command's data
until a GET_STATE (reg 0x03) completes.  Calling GET_STATUS2 immediately after
GET_INFO or GET_SERIAL therefore yields a response whose bytes[6-16] are stale,
even though byte[3] correctly echoes 0x08.  The ZWO library avoids this by
always calling GET_STATE before GET_STATUS2 in its open sequence.

```
Response byte  Field
─────────────  ─────────────────────────────────────────────
[3]            0x08 (echo)
[4]            beep_enabled  (0 = off, 1 = on)
[5]            reverse_enabled  (0 = forward, 1 = reverse)
[6..9]         current_angle_u32  (BE32, ×10000°)  — same encoding as GET_STATE
[10..12]       0x00 (reserved / unobserved)
[13..14]       max_angle_u16  (BE16, integer degrees)
[15..16]       0x00
```

Note: `CAAGetBeep()`, `CAAGetReverse()`, and `CAAGetMaxDegree()` read from
the library's internal cache (last observed STATUS2/STATE values); they do not
generate USB traffic.

---

#### Register 0x04 — GET_INFO

Returns firmware version and device type. Called once on open.

```
Response byte  Field
─────────────  ─────────────────────────────────────────────
[3]            0x04 (echo)
[4]            firmware_major
[5]            firmware_minor
[6]            firmware_patch
[7]            device_class byte (observed 0x45 = 'E'; library strips this)
[8..15]        type_string, null-terminated ASCII (e.g. "CAA-M54\0")
[16]           0x00
```

Example: `01 7E 5A 04 01 01 01 45 43 41 41 2D 4D 35 34 00 00`
→ firmware 1.1.1, type "CAA-M54"

---

#### Register 0x0C — GET_SERIAL

Returns the 8-byte serial number. Called once on open.

```
Response byte  Field
─────────────  ─────────────────────────────────────────────
[3]            0x0C (echo)
[4..11]        serial_number[8]  (printed as 16 hex chars)
[12..14]       device_suffix ASCII (e.g. "M54" = 4D 35 34)
[15..16]       0x00
```

Example: `01 7E 5A 0C 06 00 30 ED A0 CB FA 7C 4D 35 34 00 00`
→ serial `060030EDA0CBFA7C`

---

#### Register 0x0D — unknown

Always returns 17 zero bytes (after the header). Purpose unknown.

---

### CMD 0x03 — MOVE / CONFIG

Sub-type byte[4] determines the operation.

---

#### Sub-type 0x01 — MOVE_ABSOLUTE

Move to an absolute angle. **No response.** The caller polls GET_STATE to track
completion.

```
Byte   Field
────   ─────────────────────────────────────────────────────
[3]    0x03
[4]    0x01
[5]    0x00
[6..9] target_angle_u32  (BE32, ×10000°)
[10..13] 0x00
[14..15] max_angle_u16  (BE16, integer degrees — current configured max)
```

**`CAAMove()` (relative) is implemented by the library as
`CAAMoveTo(current + delta)` — the same MOVE_ABSOLUTE packet is sent; no
relative-move opcode exists on the wire.**

**`CAAMoveToMechanical()` also sends MOVE_ABSOLUTE** (the library applies any
logical↔mechanical offset internally, if one has been set via `CAACurDegree()`).

Examples:
- `CAAMoveTo(9.90°)`:  `03 7E 5A 03 01 00 00 01 82 B8 00 00 00 00 01 68`
- `CAAMove(+3.0°)` from 4.90°:  `03 7E 5A 03 01 00 00 01 34 98 00 00 00 00 01 68`
- `CAAMoveToMechanical(11.04°)`: `03 7E 5A 03 01 00 00 01 AF 40 00 00 00 00 01 68`

---

#### Sub-type 0x02 (zeros) — STOP

Halts motion immediately. **No response.** The motor decelerates to a stop;
final position is read via GET_STATE.

```
03 7E 5A 03 02 00 00 00 00 00 00 00 00 00 00 00
```

---

#### Sub-type 0x00 / 0x02 with parameters — INIT_PARAMS / SET_CONFIG

Seen in two contexts: (a) during device open/CAAGetProperty init, (b) during
CAAClose save sequence. Appear as a pair: subtype 0x02 then 0x00, same payload.
Also used by `CAASetMaxDegree()`.

```
Byte     Field
────     ──────────────────────────────────────────────────
[3]      0x03
[4]      0x02 (first of pair) or 0x00 (second of pair)
[5]      0x00
[6..7]   0x0B 0xB8 = 3000  (speed / rate parameter; constant in all observed captures)
[8..9]   0x00 0x00
[10]     0x02 (observed in SetMaxDegree) or 0x00 (init/close)
[11..13] 0x00
[14..15] max_angle_u16  (BE16, integer degrees)
```

`CAASetMaxDegree(angle)` is confirmed to encode `angle` in bytes[14..15] and
updates GET_STATE/GET_STATUS2 responses immediately:

- `CAASetMaxDegree(180.0)`: bytes[14..15] = `00 B4` (180)
- `CAASetMaxDegree(360.0)`: bytes[14..15] = `01 68` (360)

The `0x0BB8` (3000) and `0x02` values in this packet are constant across all
captures; their exact semantics are not yet decoded (possibly motor speed and
an operational mode flag).

---

### CMD 0x07 — SET_BEEP

Enables or disables the beep that sounds when motion begins. **No response.**
The library verifies the write by polling GET_STATUS2 byte[4].

```
03 7E 5A 07 [enable] 00 00 00 00 00 00 00 00 00 00 00
```

| `enable` | Meaning |
|---|---|
| `0x00` | Beep off |
| `0x01` | Beep on  |

`CAAGetBeep()` reads from cache (no USB traffic).

---

### CMD 0x09 — SET_REVERSE

Sets motor rotation direction. **No response.**
The library verifies by polling GET_STATUS2 byte[5].

```
03 7E 5A 09 [enable] 00 00 00 00 00 00 00 00 00 00 00
```

| `enable` | Meaning |
|---|---|
| `0x00` | Normal direction |
| `0x01` | Reverse direction |

`CAAGetReverse()` reads from cache (no USB traffic).

---

## CAACurDegree — no USB traffic

`CAACurDegree(id, angle)` sets the logical-position origin (a zero-point offset).
It is implemented entirely inside the library with no USB packet; it updates an
internal float used to convert between logical and mechanical angles for
subsequent `CAAMoveTo()` calls.

---

## CAAGetTemp — sensor absent on CAA-M54 firmware 1.1.1

`CAAGetTemp()` returns `CAA_ERROR_GENERAL_ERROR` (-273°C) on this device.
Disassembly of `libCAARotator` reveals why:

### Library internals

`CAAGetTemp` → `CCAA::getTempEPf` → `CCAA::getParams2Ev` → `CCAA::clearErrorEv`

`clearErrorEv` sends `CMD_QUERY / REG_STATE` (reg 0x03) and reads the response
into the internal receive buffer. `getParams2Ev` then reads bytes[11-12] of that
GET_STATE response as a big-endian uint16 "raw ADC" value.

On firmware 1.1.1 (and confirmed by live capture), GET_STATE bytes[10-12] are
always `0x00 0x00 0x00`. `raw_adc = 0`. Two code paths both fail with 0:

| Path | Condition | Formula | 0 → result |
|------|-----------|---------|------------|
| Linear (digital sensor) | `flag_ntc == 0` | `temp = raw_adc / 100.0 − 300.0` | −300°C ≤ −200°C → error |
| NTC thermistor | `flag_ntc != 0` | lookup-table interpolation | valid range 616–1020 → 0 out of range → error |

### NTC thermistor code path

The library embeds a 192-entry lookup table (`libCAARotator.so.1` .rodata at
`0x3e600`, 16 bytes/entry):
- Entry format: `int32_le(temp_C) | 0x00000000 | float64_le(adc_ratio)`
- `adc_ratio = raw_adc × 10 / (1024 − raw_adc)`  (resistor-divider on 10-bit ADC)
- Temperature range: **−20°C to +171°C**
- At 25°C: expected raw ADC ≈ 931 (10 kΩ NTC / 10 kΩ fixed on 3.3 V rail)

`flag_ntc` is set during `CAAOpen` if `obj[112]` ≠ 30000; the value 30000 is
the default/no-sensor sentinel (it is the raw ADC that the linear formula
would need to yield 0°C, making it a safe "uninitialized" marker).

### Linear / digital sensor code path

`temp = raw_adc / 100.0 − 300.0` — consistent with a digital sensor (e.g.
I²C LM75 / DS18B20) reporting temperature as `(temp + 300) × 100`.

### Probe of all registers for temperature

Every CMD_QUERY register (0x03 – 0x10, inclusive, excluding known registers)
was probed on this device. All return zeros in bytes[4-16] except:

- **0x06**: byte[4] = `0x01` — purpose unknown (motor mode flag? status?)
- **0x0D**: all zeros — confirmed not a temperature register

### Conclusion

**The CAA-M54 with firmware 1.1.1 does not have a functional temperature
sensor.** The firmware returns `0x00 0x00` in GET_STATE bytes[10-11], which
are the temperature ADC fields the library reads. The hardware likely omits the
sensor entirely; the library code is shared with devices (possibly newer CAA
models) that include either an NTC thermistor or a digital I²C sensor.

---

## Open / close sequence

### CAAOpen() (second session — actual use)

1. Open `/dev/hidraw3` `O_RDWR`
2. Open `/dev/hidraw3` `O_RDONLY|O_CLOEXEC` → `HIDIOCGRDESCSIZE` + `HIDIOCGRDESC`, then close
3. `QUERY 0x03` × 2 — initial position + motion status
4. `QUERY 0x08` — beep/reverse/position
5. `QUERY 0x04` — firmware + type
6. `QUERY 0x03` — position
7. `QUERY 0x08` — beep/reverse/position
8. `QUERY 0x03` × 2 — position (final pre-handoff reads)

### CAAClose()

1. `CMD 0x03, subtype 0x02, params` (INIT_PARAMS pair, first half)
2. `CMD 0x03, subtype 0x00, params` (INIT_PARAMS pair, second half)
3. Close fd

---

## Command summary table

| CMD  | Sub / Reg | Direction | API functions | Notes |
|------|-----------|-----------|---------------|-------|
| 0x02 | reg 0x03  | R | `CAAGetDegree`, `CAAIsMoving` | GET_STATE |
| 0x02 | reg 0x08  | R | internal poll | GET_STATUS2 (beep, reverse) |
| 0x02 | reg 0x04  | R | `CAAGetProperty`, `CAAGetFirmwareVersion`, `CAAGetType` | GET_INFO |
| 0x02 | reg 0x0C  | R | `CAAGetSerialNumber` | GET_SERIAL |
| 0x02 | reg 0x0D  | R | unknown | returns zeros |
| 0x03 | sub 0x01  | W | `CAAMoveTo`, `CAAMove`, `CAAMoveToMechanical` | MOVE_ABSOLUTE |
| 0x03 | sub 0x02 (zeros) | W | `CAAStop` | STOP |
| 0x03 | sub 0x00/0x02 (params) | W | `CAASetMaxDegree`, init/close | SET_CONFIG |
| 0x07 | byte[4]   | W | `CAASetBeep` | SET_BEEP |
| 0x09 | byte[4]   | W | `CAASetReverse` | SET_REVERSE |

---

## Remaining open questions

- [ ] `CMD 0x03 subtype 0x00/0x02 params`: exact semantics of `0x0BB8` (3000) and `0x02` byte[10] — speed register? mode flag?
- [x] `Register 0x0D`: confirmed NOT a temperature register; returns all zeros on all probes.
- [ ] `GET_STATE byte[5]`: always 0x00 — reserved, or carries a flag not yet triggered?
- [x] `CAAGetTemp`: fully reverse-engineered — library reads GET_STATE bytes[10-11] as temperature ADC; firmware 1.1.1 leaves these zero; sensor absent on CAA-M54 hardware.
- [ ] `Register 0x06 byte[4] = 0x01`: only non-zero undocumented register; purpose unknown (motor mode? hardware capability flag?).
- [ ] Backlash setting (`_ZN4CCAA10setBacklahEi` visible in binary): no API surface in `CAA_API.h`; unknown register.
- [ ] `_Z11CAASetSpeedii` visible in binary: speed configuration not in public API; likely part of the 0x0BB8 parameter.

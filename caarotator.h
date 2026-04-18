#pragma once

/*
 * caarotator.h — Low-level HID driver for the ZWO CAA Camera Angle Adjuster
 *
 * Communicates directly via Linux hidraw ioctl.  No X2 / TheSkyX dependencies.
 * On non-Linux platforms the public API compiles but all methods return
 * CAART_ERR_UNSUPPORTED so the X2 layer can report a meaningful error without
 * conditional compilation throughout the rest of the plugin.
 *
 * Protocol summary (see PROTOCOL.md):
 *   Output report  : 16 bytes  HIDIOCSFEATURE(16)  host → device
 *   Input  report  : 17 bytes  HIDIOCGFEATURE(17)  device → host
 *   Magic           : buf[0]=0x00, buf[1]=0x7E, buf[2]=0x5A
 *   Register byte   : buf[3] in output; echoed in buf[3] of input
 */

#include <stddef.h>

// ── Error codes ─────────────────────────────────────────────────────────────
#define CAART_OK              0
#define CAART_ERR_NODEV      -1   // no matching hidraw device found
#define CAART_ERR_OPEN       -2   // failed to open hidraw node
#define CAART_ERR_IO         -3   // ioctl send/receive failure
#define CAART_ERR_NOLINK     -4   // not open
#define CAART_ERR_TIMEOUT    -5   // echo-byte retry exhausted
#define CAART_ERR_UNSUPPORTED -6  // non-Linux build

// ── ZWO CAA USB IDs ─────────────────────────────────────────────────────────
#define CAA_VID  0x03C3u
#define CAA_PID  0x1F20u

class CAARotator
{
public:
    CAARotator();
    ~CAARotator();

    // ── Device discovery ────────────────────────────────────────────────────

    // Walk /sys/class/hidraw/ looking for VID=0x03C3 PID=0x1F20.
    // Writes the first match (e.g. "/dev/hidraw0") into pathOut.
    // Returns CAART_OK or CAART_ERR_NODEV.
    static int findDevice(char* pathOut, int maxLen);

    // ── Connection ──────────────────────────────────────────────────────────

    int  open(const char* path);   // returns CAART_OK or CAART_ERR_OPEN
    void close();
    bool isOpen() const;
    const char* path() const { return m_path; }

    // ── Device information ───────────────────────────────────────────────────
    // Call after open().  All out-pointers must be non-NULL.
    //   fwOut      : at least 16 bytes  ("1.1.1" etc.)
    //   typeOut    : at least 16 bytes  ("CAA-M54" etc.)
    //   serialOut  : at least 24 bytes  (hex string)
    int getInfo(char* fwOut, char* typeOut, char* serialOut);

    // ── State query ──────────────────────────────────────────────────────────

    struct State {
        double  angle;       // current logical position in degrees
        double  maxAngle;    // configured max-angle limit
        bool    isMoving;
        bool    beep;
        bool    reverse;
    };

    // Performs GET_STATE then GET_STATUS2 (matching ZWO library open sequence).
    // Populates all fields of *s.  Returns CAART_OK or error.
    int getState(State& s);

    // ── Motion ───────────────────────────────────────────────────────────────

    // Move to absolute logical angle [0, maxAngle].
    int moveTo(double degrees);

    // Stop any in-progress motion.
    int stop();

    // ── Settings ─────────────────────────────────────────────────────────────

    int setBeep(bool enabled);
    int setReverse(bool enabled);
    int setMaxDegree(double degrees);

private:
    int           m_fd;
    char          m_path[64];
    unsigned short m_cachedMaxAngle;  // updated on every getState(); used by moveTo()

    // ── HID I/O primitives ────────────────────────────────────────────────────

    // Build a 16-byte output buffer from a register + payload.
    // reg: register byte (e.g. 0x01 for GET_STATE)
    // payload / payloadLen: optional data bytes (may be NULL / 0)
    static void buildOut(unsigned char* buf16,
                         unsigned char  reg,
                         const unsigned char* payload = 0,
                         int payloadLen = 0);

    // Send buf16 via HIDIOCSFEATURE(16).
    int sendReport(const unsigned char* buf16);

    // Send reg/payload and receive a 17-byte response.
    // Retries HIDIOCGFEATURE up to 8 times while the echo byte doesn't match.
    int query(unsigned char reg,
              const unsigned char* payload, int payloadLen,
              unsigned char* resp17);

    // Convenience: query with no payload.
    int querySimple(unsigned char reg, unsigned char* resp17);

    // ── Register-level commands ───────────────────────────────────────────────

    // GET_STATE  (0x01) — returns is_moving, angle, error code
    int cmdGetState(unsigned char* resp17);

    // GET_STATUS2 (0x08) — returns beep, reverse, max_angle
    int cmdGetStatus2(unsigned char* resp17);

    // GET_INFO    (0x04) — returns firmware version + type string
    int cmdGetInfo(unsigned char* resp17a, unsigned char* resp17b);

    // GET_SERIAL  (0x09) — returns serial number bytes
    int cmdGetSerial(unsigned char* resp17);

    // MOVE_TO     (0x02) — absolute move
    int cmdMoveTo(float degrees);

    // STOP        (0x03) — stop motion
    int cmdStop();

    // SET_BEEP    (0x0C) — set beep on/off
    int cmdSetBeep(bool enabled);

    // SET_REVERSE (0x0B) — set reverse on/off
    int cmdSetReverse(bool enabled);

    // SET_MAX_DEG (0x0A) — set max degree
    int cmdSetMaxDeg(float degrees);
};

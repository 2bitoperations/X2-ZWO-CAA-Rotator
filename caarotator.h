#pragma once

/*
 * caarotator.h — Low-level HID driver for the ZWO CAA Camera Angle Adjuster
 *
 * Uses hidapi for cross-platform HID communication (Linux/macOS/Windows).
 * No X2 / TheSkyX dependencies.
 *
 * Protocol summary (see PROTOCOL.md):
 *   Output report  : 16 bytes  hid_send_feature_report()  host → device
 *   Input  report  : 17 bytes  hid_get_feature_report()   device → host
 *   Magic          : buf[1]=0x7E, buf[2]=0x5A
 *   Register byte  : buf[3] in output; echoed in buf[3] of input
 */

#include <hidapi.h>

// ── Error codes ───────────────────────────────────────────────────────────────
#define CAART_OK           0
#define CAART_ERR_NODEV   -1   // no matching device found
#define CAART_ERR_OPEN    -2   // hid_open_path() failed
#define CAART_ERR_IO      -3   // hid_send/get_feature_report failed
#define CAART_ERR_NOLINK  -4   // device not open
#define CAART_ERR_TIMEOUT -5   // echo-byte retry exhausted

// ── ZWO CAA USB IDs ──────────────────────────────────────────────────────────
#define CAA_VID  0x03C3u
#define CAA_PID  0x1F20u

// ── Device enumeration ────────────────────────────────────────────────────────

// Information about one connected CAA device, returned by enumerateDevices().
// serial is populated by briefly opening the device and reading GET_SERIAL.
struct CAADeviceEntry {
    char path[256];       // hidapi path (opaque, OS-specific)
    char serial[24];      // 16 hex chars from GET_SERIAL, or "" if unreadable
    char firmware[16];    // "major.minor.patch" from GET_INFO, or ""
    char type[16];        // "CAA-M54" etc. from GET_INFO, or ""
};

class CAARotator
{
public:
    CAARotator();
    ~CAARotator();

    // ── Device discovery ──────────────────────────────────────────────────────

    // Enumerate all connected ZWO CAA devices.
    // Each entry is briefly opened to read serial/firmware/type.
    // Returns the number of entries written (≤ maxCount).
    static int enumerateDevices(CAADeviceEntry* out, int maxCount);

    // ── Connection ────────────────────────────────────────────────────────────

    // Open device by hidapi path (from CAADeviceEntry::path or stored INI value).
    int  open(const char* path);
    void close();
    bool isOpen() const;
    const char* path() const { return m_path; }

    // ── Device information ────────────────────────────────────────────────────
    // Call after open().  Each output buffer must be at least the indicated size.
    //   fwOut    : 16 bytes  ("1.1.1")
    //   typeOut  : 16 bytes  ("CAA-M54")
    //   serialOut: 24 bytes  (16 hex chars)
    int getInfo(char* fwOut, char* typeOut, char* serialOut);

    // ── State query ───────────────────────────────────────────────────────────

    struct State {
        double  angle;       // current logical position, degrees
        double  maxAngle;    // configured max-angle limit, degrees
        bool    isMoving;
        bool    beep;
        bool    reverse;
    };

    // GET_STATE then GET_STATUS2 (ZWO library open sequence — flushes stale buffer).
    // Populates all fields.  Returns CAART_OK or error.
    int getState(State& s);

    // ── Motion ────────────────────────────────────────────────────────────────

    int moveTo(double degrees);
    int stop();

    // ── Settings ──────────────────────────────────────────────────────────────

    int setBeep(bool enabled);
    int setReverse(bool enabled);
    int setMaxDegree(double degrees);

private:
    hid_device*    m_dev;
    char           m_path[256];
    unsigned short m_cachedMaxAngle;   // updated by getState(); used by moveTo()

    // ── HID I/O primitives ────────────────────────────────────────────────────

    // Build a 16-byte output buffer.
    // buf[0]=REPORT_OUT(0x03), [1]=0x7E, [2]=0x5A, [3]=cmd, [4..]=params
    static void buildOut(unsigned char* buf16,
                         unsigned char  cmd,
                         const unsigned char* params = 0,
                         int paramsLen = 0);

    // hid_send_feature_report wrapper.
    int sendReport(const unsigned char* buf16);

    // Send CMD_QUERY(0x02)/reg and receive a 17-byte response.
    // Retries hid_get_feature_report up to 8× while echo byte != reg.
    int query(unsigned char reg, unsigned char* resp17);

    // ── Register-level commands ───────────────────────────────────────────────
    int cmdGetState(unsigned char* resp17);
    int cmdGetStatus2(unsigned char* resp17);
    int cmdGetInfo(unsigned char* resp17);
    int cmdGetSerial(unsigned char* resp17);
    int cmdMoveTo(float degrees);
    int cmdStop();
    int cmdSetBeep(bool enabled);
    int cmdSetReverse(bool enabled);
    int cmdSetMaxDeg(float degrees);
};

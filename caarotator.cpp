/*
 * caarotator.cpp — Low-level HID driver for the ZWO CAA Camera Angle Adjuster
 *
 * Protocol reference: PROTOCOL.md
 *
 * All communication is via HID feature reports through hidapi:
 *   hid_send_feature_report(dev, buf, 16) : host → device
 *   hid_get_feature_report(dev, buf, 17)  : device → host
 *
 * Output format:  [0x03][0x7E][0x5A][CMD][params 0-11]
 * Input  format:  [0x01][0x7E][0x5A][echo][data 0-12]
 */

#include "caarotator.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
static void sleepMs(int ms) { Sleep((DWORD)ms); }
#else
#  include <unistd.h>
static void sleepMs(int ms) { usleep((useconds_t)ms * 1000u); }
#endif

// ── Protocol constants ────────────────────────────────────────────────────────
#define REPORT_OUT  0x03u
#define REPORT_IN   0x01u
#define MAGIC0      0x7Eu
#define MAGIC1      0x5Au

#define CMD_QUERY       0x02u
#define CMD_MOVE        0x03u
#define CMD_SET_BEEP    0x07u
#define CMD_SET_REVERSE 0x09u

#define REG_STATE    0x03u
#define REG_STATUS2  0x08u
#define REG_INFO     0x04u
#define REG_SERIAL   0x0Cu

#define MOVE_ABSOLUTE 0x01u
#define MOVE_STOP     0x02u
#define MOVE_CONFIG   0x00u

// ── Angle encoding ────────────────────────────────────────────────────────────

static unsigned int encodeAngle(double deg)
{
    long long v = (long long)(deg * 10000.0 + 0.5);
    if (v < 0) v = 0;
    return (unsigned int)v;
}

static double decodeAngle(const unsigned char* buf, int offset)
{
    unsigned int v = ((unsigned int)buf[offset]   << 24)
                   | ((unsigned int)buf[offset+1] << 16)
                   | ((unsigned int)buf[offset+2] <<  8)
                   | ((unsigned int)buf[offset+3]);
    return v / 10000.0;
}

static unsigned short decodeU16(const unsigned char* buf, int offset)
{
    return (unsigned short)(((unsigned int)buf[offset] << 8) | buf[offset+1]);
}

// ════════════════════════════════════════════════════════════════════════════
// CAARotator
// ════════════════════════════════════════════════════════════════════════════

CAARotator::CAARotator()
    : m_dev(NULL)
    , m_cachedMaxAngle(360)
{
    m_path[0] = '\0';
}

CAARotator::~CAARotator()
{
    close();
}

bool CAARotator::isOpen() const
{
    return m_dev != NULL;
}

// ── Device enumeration ────────────────────────────────────────────────────────

int CAARotator::enumerateDevices(CAADeviceEntry* out, int maxCount)
{
    if (!out || maxCount <= 0)
        return 0;

    struct hid_device_info* devs = hid_enumerate(CAA_VID, CAA_PID);
    if (!devs)
        return 0;

    int count = 0;
    for (struct hid_device_info* d = devs; d && count < maxCount; d = d->next) {
        CAADeviceEntry& e = out[count];
        memset(&e, 0, sizeof(e));
        snprintf(e.path, sizeof(e.path), "%s", d->path ? d->path : "");

        // Briefly open to read serial and firmware info
        hid_device* dev = hid_open_path(d->path);
        if (dev) {
            // GET_STATE flush first (clears stale buffer)
            unsigned char buf[17];
            buf[0] = REPORT_IN;
            memset(buf + 1, 0, 16);
            buf[3] = REG_STATE;
            // Send QUERY REG_STATE
            unsigned char out16[16];
            memset(out16, 0, 16);
            out16[0] = REPORT_OUT;
            out16[1] = MAGIC0;
            out16[2] = MAGIC1;
            out16[3] = CMD_QUERY;
            out16[4] = REG_STATE;
            hid_send_feature_report(dev, out16, 16);
            buf[0] = REPORT_IN;
            hid_get_feature_report(dev, buf, 17);

            // GET_INFO
            memset(out16 + 4, 0, 12);
            out16[4] = REG_INFO;
            hid_send_feature_report(dev, out16, 16);
            unsigned char infoResp[17];
            infoResp[0] = REPORT_IN;
            for (int i = 0; i < 8; i++) {
                hid_get_feature_report(dev, infoResp, 17);
                if (infoResp[3] == REG_INFO) break;
                sleepMs(50);
            }
            if (infoResp[3] == REG_INFO) {
                snprintf(e.firmware, sizeof(e.firmware), "%d.%d.%d",
                         (int)infoResp[4], (int)infoResp[5], (int)infoResp[6]);
                memcpy(e.type, infoResp + 8, 8);
                e.type[8] = '\0';
            }

            // GET_SERIAL (flush state first)
            out16[4] = REG_STATE;
            hid_send_feature_report(dev, out16, 16);
            buf[0] = REPORT_IN;
            hid_get_feature_report(dev, buf, 17);

            out16[4] = REG_SERIAL;
            hid_send_feature_report(dev, out16, 16);
            unsigned char snResp[17];
            snResp[0] = REPORT_IN;
            for (int i = 0; i < 8; i++) {
                hid_get_feature_report(dev, snResp, 17);
                if (snResp[3] == REG_SERIAL) break;
                sleepMs(50);
            }
            if (snResp[3] == REG_SERIAL) {
                for (int i = 0; i < 8; i++)
                    snprintf(e.serial + i*2, 3, "%02X", (unsigned int)snResp[4+i]);
                e.serial[16] = '\0';
            }

            hid_close(dev);
        }

        count++;
    }

    hid_free_enumeration(devs);
    return count;
}

// ── Connection ────────────────────────────────────────────────────────────────

int CAARotator::open(const char* path)
{
    close();

    hid_device* dev = hid_open_path(path);
    if (!dev)
        return CAART_ERR_OPEN;

    m_dev = dev;
    snprintf(m_path, sizeof(m_path), "%s", path);
    return CAART_OK;
}

void CAARotator::close()
{
    if (m_dev) {
        hid_close(m_dev);
        m_dev = NULL;
        m_path[0] = '\0';
    }
}

// ── HID I/O primitives ────────────────────────────────────────────────────────

void CAARotator::buildOut(unsigned char* buf16,
                          unsigned char  cmd,
                          const unsigned char* params,
                          int paramsLen)
{
    memset(buf16, 0, 16);
    buf16[0] = REPORT_OUT;
    buf16[1] = MAGIC0;
    buf16[2] = MAGIC1;
    buf16[3] = cmd;
    if (params && paramsLen > 0) {
        int n = paramsLen < 12 ? paramsLen : 12;
        memcpy(buf16 + 4, params, (size_t)n);
    }
}

int CAARotator::sendReport(const unsigned char* buf16)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;
    int rc = hid_send_feature_report(m_dev, buf16, 16);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
}

int CAARotator::query(unsigned char reg, unsigned char* resp17)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;

    // Send CMD_QUERY with the register number in byte[4]
    unsigned char out[16];
    buildOut(out, CMD_QUERY);
    out[4] = reg;

    int rc = hid_send_feature_report(m_dev, out, 16);
    if (rc < 0)
        return CAART_ERR_IO;

    // Retry hid_get_feature_report until echo byte matches.
    // GET_INFO and GET_SERIAL are slow registers — device updates echo quickly
    // but leaves data bytes stale from the previous command (see PROTOCOL.md).
    resp17[0] = REPORT_IN;
    for (int i = 0; i < 8; i++) {
        rc = hid_get_feature_report(m_dev, resp17, 17);
        if (rc < 0)
            return CAART_ERR_IO;
        if (resp17[3] == reg)
            return CAART_OK;
        sleepMs(50);
    }
    return CAART_ERR_TIMEOUT;
}

// ── Register-level commands ───────────────────────────────────────────────────

int CAARotator::cmdGetState(unsigned char* resp17)   { return query(REG_STATE,   resp17); }
int CAARotator::cmdGetStatus2(unsigned char* resp17) { return query(REG_STATUS2, resp17); }
int CAARotator::cmdGetInfo(unsigned char* resp17)    { return query(REG_INFO,    resp17); }
int CAARotator::cmdGetSerial(unsigned char* resp17)  { return query(REG_SERIAL,  resp17); }

int CAARotator::cmdMoveTo(float degrees)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;

    unsigned int  ang      = encodeAngle((double)degrees);
    unsigned short maxAngle = m_cachedMaxAngle;

    unsigned char out[16];
    memset(out, 0, 16);
    out[0]  = REPORT_OUT;
    out[1]  = MAGIC0;
    out[2]  = MAGIC1;
    out[3]  = CMD_MOVE;
    out[4]  = MOVE_ABSOLUTE;
    out[5]  = 0x00;
    // bytes 6..9: target angle BE32
    out[6]  = (unsigned char)((ang >> 24) & 0xFF);
    out[7]  = (unsigned char)((ang >> 16) & 0xFF);
    out[8]  = (unsigned char)((ang >>  8) & 0xFF);
    out[9]  = (unsigned char)( ang        & 0xFF);
    // bytes 14..15: max_angle_u16 BE
    out[14] = (unsigned char)((maxAngle >> 8) & 0xFF);
    out[15] = (unsigned char)( maxAngle       & 0xFF);

    int rc = hid_send_feature_report(m_dev, out, 16);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
}

int CAARotator::cmdStop()
{
    if (!m_dev)
        return CAART_ERR_NOLINK;
    unsigned char out[16];
    memset(out, 0, 16);
    out[0] = REPORT_OUT;
    out[1] = MAGIC0;
    out[2] = MAGIC1;
    out[3] = CMD_MOVE;
    out[4] = MOVE_STOP;
    int rc = hid_send_feature_report(m_dev, out, 16);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
}

int CAARotator::cmdSetBeep(bool enabled)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;
    unsigned char out[16];
    memset(out, 0, 16);
    out[0] = REPORT_OUT;
    out[1] = MAGIC0;
    out[2] = MAGIC1;
    out[3] = CMD_SET_BEEP;
    out[4] = enabled ? 0x01u : 0x00u;
    int rc = hid_send_feature_report(m_dev, out, 16);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
}

int CAARotator::cmdSetReverse(bool enabled)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;
    unsigned char out[16];
    memset(out, 0, 16);
    out[0] = REPORT_OUT;
    out[1] = MAGIC0;
    out[2] = MAGIC1;
    out[3] = CMD_SET_REVERSE;
    out[4] = enabled ? 0x01u : 0x00u;
    int rc = hid_send_feature_report(m_dev, out, 16);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
}

int CAARotator::cmdSetMaxDeg(float degrees)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;
    unsigned short maxAngle = (unsigned short)((int)degrees);
    unsigned char out[16];
    memset(out, 0, 16);
    out[0]  = REPORT_OUT;
    out[1]  = MAGIC0;
    out[2]  = MAGIC1;
    out[3]  = CMD_MOVE;
    out[4]  = MOVE_CONFIG;   // 0x00
    // bytes 6..7: 0x0BB8 (3000) — constant speed/rate parameter
    out[6]  = 0x0Bu;
    out[7]  = 0xB8u;
    out[10] = 0x02u;         // constant mode byte observed in SetMaxDegree captures
    // bytes 14..15: max_angle BE16
    out[14] = (unsigned char)((maxAngle >> 8) & 0xFF);
    out[15] = (unsigned char)( maxAngle       & 0xFF);
    int rc = hid_send_feature_report(m_dev, out, 16);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
}

// ── High-level API ────────────────────────────────────────────────────────────

int CAARotator::getState(State& s)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;

    // GET_STATE first — flushes device response buffer
    unsigned char stateResp[17];
    stateResp[0] = REPORT_IN;
    int rc = cmdGetState(stateResp);
    if (rc != CAART_OK)
        return rc;

    // GET_STATUS2 for beep / reverse
    unsigned char s2Resp[17];
    s2Resp[0] = REPORT_IN;
    rc = cmdGetStatus2(s2Resp);
    if (rc != CAART_OK)
        return rc;

    m_cachedMaxAngle = decodeU16(stateResp, 13);
    s.isMoving = (stateResp[4] != 0);
    s.angle    = decodeAngle(stateResp, 6);
    s.maxAngle = (double)m_cachedMaxAngle;
    s.beep     = (s2Resp[4] != 0);
    s.reverse  = (s2Resp[5] != 0);

    return CAART_OK;
}

int CAARotator::getInfo(char* fwOut, char* typeOut, char* serialOut)
{
    if (!m_dev)
        return CAART_ERR_NOLINK;

    // Flush stale buffer before slow register reads
    unsigned char stateResp[17];
    stateResp[0] = REPORT_IN;
    cmdGetState(stateResp);  // best-effort; ignore error

    // GET_INFO
    unsigned char infoResp[17];
    infoResp[0] = REPORT_IN;
    int rc = cmdGetInfo(infoResp);
    if (rc != CAART_OK)
        return rc;

    snprintf(fwOut, 16, "%d.%d.%d",
             (int)infoResp[4], (int)infoResp[5], (int)infoResp[6]);
    memcpy(typeOut, infoResp + 8, 8);
    typeOut[8] = '\0';

    // Flush again before GET_SERIAL (INFO is a slow register)
    stateResp[0] = REPORT_IN;
    cmdGetState(stateResp);

    unsigned char snResp[17];
    snResp[0] = REPORT_IN;
    rc = cmdGetSerial(snResp);
    if (rc != CAART_OK) {
        serialOut[0] = '\0';
    } else {
        for (int i = 0; i < 8; i++)
            snprintf(serialOut + i*2, 3, "%02X", (unsigned int)snResp[4+i]);
        serialOut[16] = '\0';
    }

    return CAART_OK;
}

int CAARotator::moveTo(double degrees)    { return cmdMoveTo((float)degrees); }
int CAARotator::stop()                    { return cmdStop(); }
int CAARotator::setBeep(bool enabled)     { return cmdSetBeep(enabled); }
int CAARotator::setReverse(bool enabled)  { return cmdSetReverse(enabled); }

int CAARotator::setMaxDegree(double degrees)
{
    int rc = cmdSetMaxDeg((float)degrees);
    if (rc == CAART_OK)
        m_cachedMaxAngle = (unsigned short)((int)degrees);
    return rc;
}

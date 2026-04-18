/*
 * caarotator.cpp — Low-level HID driver for the ZWO CAA Camera Angle Adjuster
 *
 * Protocol reference: PROTOCOL.md
 *
 * All communication is via HID feature reports on /dev/hidrawN:
 *   HIDIOCSFEATURE(16) : host → device (16-byte output report)
 *   HIDIOCGFEATURE(17) : device → host (17-byte input report)
 *
 * Output format:  [0x03][0x7E][0x5A][CMD][params 0-11][0x00]
 * Input  format:  [0x01][0x7E][0x5A][echo][data 0-12]
 *
 * Platform note: HID ioctl is Linux-only.  On other platforms every method
 * returns CAART_ERR_UNSUPPORTED.
 */

#include "caarotator.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef SB_LINUX_BUILD
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <time.h>
#endif

// ── protocol constants ────────────────────────────────────────────────────────
#define REPORT_OUT  0x03u
#define REPORT_IN   0x01u
#define MAGIC0      0x7Eu
#define MAGIC1      0x5Au

// CMD bytes
#define CMD_QUERY       0x02u
#define CMD_MOVE        0x03u
#define CMD_SET_BEEP    0x07u
#define CMD_SET_REVERSE 0x09u

// Query sub-registers
#define REG_STATE    0x03u
#define REG_STATUS2  0x08u
#define REG_INFO     0x04u
#define REG_SERIAL   0x0Cu

// MOVE sub-types
#define MOVE_ABSOLUTE 0x01u
#define MOVE_STOP     0x02u
#define MOVE_CONFIG   0x00u

// Angle encoding: BE32, units = 1/10000 degree
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

// ── sleep helper ──────────────────────────────────────────────────────────────
static void sleepMs(int ms)
{
#ifdef SB_LINUX_BUILD
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#else
    (void)ms;
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// CAARotator
// ════════════════════════════════════════════════════════════════════════════

CAARotator::CAARotator()
    : m_fd(-1)
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
    return m_fd >= 0;
}

// ── Device discovery ──────────────────────────────────────────────────────────

int CAARotator::findDevice(char* pathOut, int maxLen)
{
#ifndef SB_LINUX_BUILD
    (void)pathOut; (void)maxLen;
    return CAART_ERR_UNSUPPORTED;
#else
    // Walk /sys/class/hidraw/hidrawN/device/uevent looking for
    //   HID_ID=0003:000003C3:00001F20
    // Sysfs always reports VID/PID in uppercase hex zero-padded to 8 digits.
    const char* sysBase = "/sys/class/hidraw";
    DIR* dir = opendir(sysBase);
    if (!dir)
        return CAART_ERR_NODEV;

    int found = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && !found) {
        if (ent->d_name[0] == '.')
            continue;

        char ueventPath[512];
        snprintf(ueventPath, sizeof(ueventPath),
                 "%s/%s/device/uevent", sysBase, ent->d_name);

        FILE* f = fopen(ueventPath, "r");
        if (!f)
            continue;

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            // Match: HID_ID=0003:000003C3:00001F20
            unsigned int bus, vid, pid;
            if (sscanf(line, "HID_ID=%04X:%08X:%08X", &bus, &vid, &pid) == 3) {
                if (vid == CAA_VID && pid == CAA_PID) {
                    snprintf(pathOut, (size_t)maxLen, "/dev/%s", ent->d_name);
                    found = 1;
                }
                break;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return found ? CAART_OK : CAART_ERR_NODEV;
#endif
}

// ── Connection ────────────────────────────────────────────────────────────────

int CAARotator::open(const char* path)
{
#ifndef SB_LINUX_BUILD
    (void)path;
    return CAART_ERR_UNSUPPORTED;
#else
    close();
    int fd = ::open(path, O_RDWR);
    if (fd < 0)
        return CAART_ERR_OPEN;
    m_fd = fd;
    snprintf(m_path, sizeof(m_path), "%s", path);
    return CAART_OK;
#endif
}

void CAARotator::close()
{
#ifdef SB_LINUX_BUILD
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
        m_path[0] = '\0';
    }
#endif
}

// ── HID I/O primitives ────────────────────────────────────────────────────────

void CAARotator::buildOut(unsigned char* buf16,
                          unsigned char  reg,
                          const unsigned char* payload,
                          int payloadLen)
{
    memset(buf16, 0, 16);
    buf16[0] = REPORT_OUT;
    buf16[1] = MAGIC0;
    buf16[2] = MAGIC1;
    buf16[3] = reg;
    if (payload && payloadLen > 0) {
        int n = payloadLen;
        if (n > 12) n = 12;
        memcpy(buf16 + 4, payload, (size_t)n);
    }
}

int CAARotator::sendReport(const unsigned char* buf16)
{
#ifndef SB_LINUX_BUILD
    (void)buf16;
    return CAART_ERR_UNSUPPORTED;
#else
    if (m_fd < 0)
        return CAART_ERR_NOLINK;
    // HIDIOCSFEATURE(n) is _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x06, n)
    int rc = ioctl(m_fd, HIDIOCSFEATURE(16), (void*)buf16);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
#endif
}

int CAARotator::query(unsigned char reg,
                      const unsigned char* payload, int payloadLen,
                      unsigned char* resp17)
{
#ifndef SB_LINUX_BUILD
    (void)reg; (void)payload; (void)payloadLen; (void)resp17;
    return CAART_ERR_UNSUPPORTED;
#else
    if (m_fd < 0)
        return CAART_ERR_NOLINK;

    unsigned char out[16];
    // CMD_QUERY (0x02) in byte[3]; register in byte[4]
    buildOut(out, CMD_QUERY, NULL, 0);
    out[4] = reg;
    if (payload && payloadLen > 0) {
        int n = payloadLen;
        if (n > 11) n = 11;
        memcpy(out + 5, payload, (size_t)n);
    }

    int rc = ioctl(m_fd, HIDIOCSFEATURE(16), out);
    if (rc < 0)
        return CAART_ERR_IO;

    // Retry HIDIOCGFEATURE up to 8 times waiting for echo byte to match.
    // Some registers (INFO, SERIAL) update the echo quickly but leave data
    // bytes stale — GET_STATE flushes that; see PROTOCOL.md timing note.
    resp17[0] = REPORT_IN;
    for (int i = 0; i < 8; i++) {
        rc = ioctl(m_fd, HIDIOCGFEATURE(17), resp17);
        if (rc < 0)
            return CAART_ERR_IO;
        if (resp17[3] == reg)
            return CAART_OK;
        sleepMs(50);
    }
    return CAART_ERR_TIMEOUT;
#endif
}

int CAARotator::querySimple(unsigned char reg, unsigned char* resp17)
{
    return query(reg, NULL, 0, resp17);
}

// ── Register-level commands ───────────────────────────────────────────────────

int CAARotator::cmdGetState(unsigned char* resp17)
{
    return querySimple(REG_STATE, resp17);
}

int CAARotator::cmdGetStatus2(unsigned char* resp17)
{
    return querySimple(REG_STATUS2, resp17);
}

int CAARotator::cmdGetInfo(unsigned char* resp17a, unsigned char* /*resp17b*/)
{
    return querySimple(REG_INFO, resp17a);
}

int CAARotator::cmdGetSerial(unsigned char* resp17)
{
    return querySimple(REG_SERIAL, resp17);
}

int CAARotator::cmdMoveTo(float degrees)
{
#ifndef SB_LINUX_BUILD
    (void)degrees;
    return CAART_ERR_UNSUPPORTED;
#else
    if (m_fd < 0)
        return CAART_ERR_NOLINK;

    unsigned short maxAngle = m_cachedMaxAngle;
    unsigned int ang = encodeAngle((double)degrees);
    unsigned char out[16];
    memset(out, 0, 16);
    out[0] = REPORT_OUT;
    out[1] = MAGIC0;
    out[2] = MAGIC1;
    out[3] = CMD_MOVE;
    out[4] = MOVE_ABSOLUTE;
    out[5] = 0x00;
    // bytes 6..9: target angle BE32
    out[6]  = (unsigned char)((ang >> 24) & 0xFF);
    out[7]  = (unsigned char)((ang >> 16) & 0xFF);
    out[8]  = (unsigned char)((ang >>  8) & 0xFF);
    out[9]  = (unsigned char)( ang        & 0xFF);
    out[10] = 0x00;
    out[11] = 0x00;
    out[12] = 0x00;
    out[13] = 0x00;
    // bytes 14..15: max_angle_u16 BE
    out[14] = (unsigned char)((maxAngle >> 8) & 0xFF);
    out[15] = (unsigned char)( maxAngle       & 0xFF);

    int rc = ioctl(m_fd, HIDIOCSFEATURE(16), out);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
#endif
}

int CAARotator::cmdStop()
{
#ifndef SB_LINUX_BUILD
    return CAART_ERR_UNSUPPORTED;
#else
    if (m_fd < 0)
        return CAART_ERR_NOLINK;
    unsigned char out[16];
    memset(out, 0, 16);
    out[0] = REPORT_OUT;
    out[1] = MAGIC0;
    out[2] = MAGIC1;
    out[3] = CMD_MOVE;
    out[4] = MOVE_STOP;
    int rc = ioctl(m_fd, HIDIOCSFEATURE(16), out);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
#endif
}

int CAARotator::cmdSetBeep(bool enabled)
{
#ifndef SB_LINUX_BUILD
    (void)enabled;
    return CAART_ERR_UNSUPPORTED;
#else
    if (m_fd < 0)
        return CAART_ERR_NOLINK;
    unsigned char out[16];
    memset(out, 0, 16);
    out[0] = REPORT_OUT;
    out[1] = MAGIC0;
    out[2] = MAGIC1;
    out[3] = CMD_SET_BEEP;
    out[4] = enabled ? 0x01u : 0x00u;
    int rc = ioctl(m_fd, HIDIOCSFEATURE(16), out);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
#endif
}

int CAARotator::cmdSetReverse(bool enabled)
{
#ifndef SB_LINUX_BUILD
    (void)enabled;
    return CAART_ERR_UNSUPPORTED;
#else
    if (m_fd < 0)
        return CAART_ERR_NOLINK;
    unsigned char out[16];
    memset(out, 0, 16);
    out[0] = REPORT_OUT;
    out[1] = MAGIC0;
    out[2] = MAGIC1;
    out[3] = CMD_SET_REVERSE;
    out[4] = enabled ? 0x01u : 0x00u;
    int rc = ioctl(m_fd, HIDIOCSFEATURE(16), out);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
#endif
}

int CAARotator::cmdSetMaxDeg(float degrees)
{
#ifndef SB_LINUX_BUILD
    (void)degrees;
    return CAART_ERR_UNSUPPORTED;
#else
    if (m_fd < 0)
        return CAART_ERR_NOLINK;
    unsigned short maxAngle = (unsigned short)((int)degrees);
    unsigned char out[16];
    memset(out, 0, 16);
    out[0]  = REPORT_OUT;
    out[1]  = MAGIC0;
    out[2]  = MAGIC1;
    out[3]  = CMD_MOVE;
    out[4]  = MOVE_CONFIG;  // 0x00
    out[5]  = 0x00;
    // bytes 6..7: 0x0BB8 (3000) — constant speed/rate parameter
    out[6]  = 0x0Bu;
    out[7]  = 0xB8u;
    out[8]  = 0x00;
    out[9]  = 0x00;
    out[10] = 0x02u;  // constant mode byte observed in all SetMaxDegree captures
    out[11] = 0x00;
    out[12] = 0x00;
    out[13] = 0x00;
    // bytes 14..15: max_angle BE16
    out[14] = (unsigned char)((maxAngle >> 8) & 0xFF);
    out[15] = (unsigned char)( maxAngle       & 0xFF);
    int rc = ioctl(m_fd, HIDIOCSFEATURE(16), out);
    return (rc < 0) ? CAART_ERR_IO : CAART_OK;
#endif
}

// ── High-level API ────────────────────────────────────────────────────────────

int CAARotator::getState(State& s)
{
    if (m_fd < 0)
        return CAART_ERR_NOLINK;

    // GET_STATE first — flushes device response buffer (PROTOCOL.md §timing note)
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
    if (m_fd < 0)
        return CAART_ERR_NOLINK;

    // Call GET_STATE first to flush any stale buffer from a previous slow register
    unsigned char stateResp[17];
    stateResp[0] = REPORT_IN;
    cmdGetState(stateResp);  // best-effort flush; ignore error here

    // GET_INFO
    unsigned char infoResp[17];
    infoResp[0] = REPORT_IN;
    int rc = cmdGetInfo(infoResp, NULL);
    if (rc != CAART_OK)
        return rc;

    snprintf(fwOut, 16, "%d.%d.%d",
             (int)infoResp[4], (int)infoResp[5], (int)infoResp[6]);

    // byte[7] is device-class byte; type string at [8..15]
    memset(typeOut, 0, 16);
    memcpy(typeOut, infoResp + 8, 8);
    typeOut[8] = '\0';

    // GET_SERIAL
    // Flush state first (INFO is a slow register; see timing note)
    stateResp[0] = REPORT_IN;
    cmdGetState(stateResp);

    unsigned char snResp[17];
    snResp[0] = REPORT_IN;
    rc = querySimple(REG_SERIAL, snResp);
    if (rc != CAART_OK) {
        serialOut[0] = '\0';
    } else {
        for (int i = 0; i < 8; i++)
            snprintf(serialOut + i*2, 3, "%02X", (unsigned int)snResp[4+i]);
        serialOut[16] = '\0';
    }

    return CAART_OK;
}

int CAARotator::moveTo(double degrees)
{
    return cmdMoveTo((float)degrees);
}

int CAARotator::stop()
{
    return cmdStop();
}

int CAARotator::setBeep(bool enabled)
{
    return cmdSetBeep(enabled);
}

int CAARotator::setReverse(bool enabled)
{
    return cmdSetReverse(enabled);
}

int CAARotator::setMaxDegree(double degrees)
{
    int rc = cmdSetMaxDeg((float)degrees);
    if (rc == CAART_OK)
        m_cachedMaxAngle = (unsigned short)((int)degrees);
    return rc;
}

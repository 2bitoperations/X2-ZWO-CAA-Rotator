/*
 * x2caarotator.cpp — X2 Rotator plugin for the ZWO CAA Camera Angle Adjuster
 */

#include "x2caarotator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define LOCK_IO  X2MutexLocker _lock(m_pIOMutex)

// INI keys (section = X2CAART_INI_SECTION)
#define KEY_SERIAL      "DeviceSerial"
#define KEY_PATH        "DevicePath"
#define KEY_BEEP        "Beep"
#define KEY_REVERSE     "Reverse"
#define KEY_MAX_ANGLE   "MaxAngle"
#define KEY_DEBUG_LEVEL "DebugLevel"

// ── Construction ──────────────────────────────────────────────────────────────

X2CAARotator::X2CAARotator(
    const char*                         /*pszDisplayName*/,
    const int&                          nInstanceIndex,
    SerXInterface*                      pSerXIn,
    TheSkyXFacadeForDriversInterface*   pTheSkyXIn,
    SleeperInterface*                   pSleeperIn,
    BasicIniUtilInterface*              pIniUtilIn,
    LoggerInterface*                    pLoggerIn,
    MutexInterface*                     pIOMutexIn,
    TickCountInterface*                 pTickCountIn)
    : m_pSerX       (pSerXIn)
    , m_pTheSkyX    (pTheSkyXIn)
    , m_pSleeper    (pSleeperIn)
    , m_pIniUtil    (pIniUtilIn)
    , m_pLogger     (pLoggerIn)
    , m_pIOMutex    (pIOMutexIn)
    , m_pTickCount  (pTickCountIn)
    , m_nInstanceIndex(nInstanceIndex)
    , m_bLinked     (false)
    , m_nDebugLevel (0)
    , m_bDoingGoto  (false)
    , m_dTargetPosition(0.0)
    , m_dCurrentPosition(0.0)
    , m_bBeep       (true)
    , m_bReverse    (false)
    , m_dMaxAngle   (360.0)
{
    m_szFirmware[0]     = '\0';
    m_szType[0]         = '\0';
    m_szSerial[0]       = '\0';
    m_szDevPath[0]      = '\0';
    m_szStoredSerial[0] = '\0';
    m_szStoredPath[0]   = '\0';

    loadSettings();
}

X2CAARotator::~X2CAARotator()
{
    terminateLink();
    // X2 framework owns all interface pointers — do not delete them
}

// ── queryAbstraction ──────────────────────────────────────────────────────────

int X2CAARotator::queryAbstraction(const char* pszName, void** ppVal)
{
    *ppVal = NULL;

    if (!strcmp(pszName, ModalSettingsDialogInterface_Name))
        *ppVal = dynamic_cast<ModalSettingsDialogInterface*>(this);
    else if (!strcmp(pszName, X2GUIEventInterface_Name))
        *ppVal = dynamic_cast<X2GUIEventInterface*>(this);

    return SB_OK;
}

// ── DriverInfoInterface ───────────────────────────────────────────────────────

void X2CAARotator::driverInfoDetailedInfo(BasicStringInterface& str) const
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "ZWO CAA Rotator X2 Plugin v" PLUGIN_VERSION_STRING
             " (" GIT_HASH ")");
    str = buf;
}

// ── HardwareInfoInterface ─────────────────────────────────────────────────────

void X2CAARotator::deviceInfoNameShort(BasicStringInterface& str) const
{
    str = "ZWO CAA Rotator";
}

void X2CAARotator::deviceInfoNameLong(BasicStringInterface& str) const
{
    str = "ZWO CAA Camera Angle Adjuster";
}

void X2CAARotator::deviceInfoDetailedDescription(BasicStringInterface& str) const
{
    str = "ZWO CAA Camera Angle Adjuster — USB HID rotator";
}

void X2CAARotator::deviceInfoFirmwareVersion(BasicStringInterface& str)
{
    str = m_szFirmware[0] ? m_szFirmware : "unknown";
}

void X2CAARotator::deviceInfoModel(BasicStringInterface& str)
{
    str = m_szType[0] ? m_szType : "ZWO CAA";
}

// ── Device selection ──────────────────────────────────────────────────────────

void X2CAARotator::makeDeviceLabel(const CAADeviceEntry& e,
                                   char* buf, int bufLen)
{
    // e.g. "CAA-M54 [SN: 060030EDA0CBFA7C]  /dev/hidraw0"
    // or   "CAA [no serial]  /dev/hidraw0"
    char snPart[32];
    if (e.serial[0])
        snprintf(snPart, sizeof(snPart), "[SN: %s]", e.serial);
    else
        snprintf(snPart, sizeof(snPart), "[no serial]");

    const char* typePart = e.type[0] ? e.type : "ZWO CAA";

    snprintf(buf, (size_t)bufLen, "%s %s  %s", typePart, snPart, e.path);
}

int X2CAARotator::chooseDevice(char* pathOut, int pathOutLen) const
{
    CAADeviceEntry devs[X2CAART_MAX_DEVICES];
    int n = CAARotator::enumerateDevices(devs, X2CAART_MAX_DEVICES);
    if (n == 0)
        return CAART_ERR_NODEV;

    // 1. Match by stored serial number (preferred — stable across USB reconnects)
    if (m_szStoredSerial[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(devs[i].serial, m_szStoredSerial) == 0) {
                snprintf(pathOut, (size_t)pathOutLen, "%s", devs[i].path);
                logDebug(2, "chooseDevice: matched by serial %s → %s",
                         m_szStoredSerial, devs[i].path);
                return CAART_OK;
            }
        }
        logDebug(1, "chooseDevice: serial %s not found, trying path hint",
                 m_szStoredSerial);
    }

    // 2. Match by stored path (hint — may be stale after USB reconnect)
    if (m_szStoredPath[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(devs[i].path, m_szStoredPath) == 0) {
                snprintf(pathOut, (size_t)pathOutLen, "%s", devs[i].path);
                logDebug(2, "chooseDevice: matched by path %s", devs[i].path);
                return CAART_OK;
            }
        }
        logDebug(1, "chooseDevice: path %s not found, using first device",
                 m_szStoredPath);
    }

    // 3. Fall back to first available device
    snprintf(pathOut, (size_t)pathOutLen, "%s", devs[0].path);
    logDebug(2, "chooseDevice: using first device %s", devs[0].path);
    return CAART_OK;
}

// ── LinkInterface ─────────────────────────────────────────────────────────────

int X2CAARotator::establishLink()
{
    LOCK_IO;

    if (m_bLinked)
        return SB_OK;

    char path[256];
    if (chooseDevice(path, (int)sizeof(path)) != CAART_OK) {
        logDebug(1, "establishLink: no ZWO CAA device found");
        return ERR_COMMNOLINK;
    }

    int rc = m_dev.open(path);
    if (rc != CAART_OK) {
        logDebug(1, "establishLink: open(%s) failed (rc=%d)", path, rc);
        return ERR_COMMOPENING;
    }

    // Read device information (includes buffer flush internally)
    char fw[16], type[16], serial[24];
    rc = m_dev.getInfo(fw, type, serial);
    if (rc == CAART_OK) {
        snprintf(m_szFirmware, sizeof(m_szFirmware), "%s", fw);
        snprintf(m_szType,     sizeof(m_szType),     "%s", type);
        snprintf(m_szSerial,   sizeof(m_szSerial),   "%s", serial);
    }

    // Read live state
    CAARotator::State s;
    rc = m_dev.getState(s);
    if (rc == CAART_OK) {
        m_dCurrentPosition = s.angle;
        m_bBeep            = s.beep;
        m_bReverse         = s.reverse;
        m_dMaxAngle        = s.maxAngle;
    }

    snprintf(m_szDevPath, sizeof(m_szDevPath), "%s", path);

    // Persist the serial and path for next connect
    if (m_szSerial[0]) {
        snprintf(m_szStoredSerial, sizeof(m_szStoredSerial), "%s", m_szSerial);
        snprintf(m_szStoredPath,   sizeof(m_szStoredPath),   "%s", path);
        saveSettings();
    }

    m_bLinked = true;

    logDebug(2, "establishLink: %s  fw=%s  type=%s  sn=%s  pos=%.4f  max=%.0f",
             path, m_szFirmware, m_szType, m_szSerial,
             m_dCurrentPosition, m_dMaxAngle);

    return SB_OK;
}

int X2CAARotator::terminateLink()
{
    if (!m_bLinked)
        return SB_OK;

    m_bDoingGoto = false;
    m_dev.close();
    m_bLinked = false;
    logDebug(2, "terminateLink: closed");
    return SB_OK;
}

// ── RotatorDriverInterface ────────────────────────────────────────────────────

int X2CAARotator::position(double& dPosition)
{
    if (!m_bLinked)
        return ERR_COMMNOLINK;

    LOCK_IO;

    CAARotator::State s;
    int rc = m_dev.getState(s);
    if (rc != CAART_OK) {
        logDebug(1, "position: getState failed (rc=%d)", rc);
        return ERR_CMDFAILED;
    }
    m_dCurrentPosition = s.angle;
    dPosition = s.angle;
    return SB_OK;
}

int X2CAARotator::abort()
{
    if (!m_bLinked)
        return ERR_COMMNOLINK;

    LOCK_IO;

    m_bDoingGoto = false;
    int rc = m_dev.stop();
    if (rc != CAART_OK) {
        logDebug(1, "abort: stop failed (rc=%d)", rc);
        return ERR_CMDFAILED;
    }
    logDebug(2, "abort: stop issued");
    return SB_OK;
}

int X2CAARotator::startRotatorGoto(const double& dTargetPosition)
{
    if (!m_bLinked)
        return ERR_COMMNOLINK;

    if (m_bDoingGoto)
        return ERR_COMMANDINPROGRESS;

    LOCK_IO;

    int rc = m_dev.moveTo(dTargetPosition);
    if (rc != CAART_OK) {
        logDebug(1, "startRotatorGoto(%.4f): moveTo failed (rc=%d)",
                 dTargetPosition, rc);
        return ERR_CMDFAILED;
    }

    m_dTargetPosition = dTargetPosition;
    m_bDoingGoto      = true;
    logDebug(2, "startRotatorGoto: target=%.4f", dTargetPosition);
    return SB_OK;
}

int X2CAARotator::isCompleteRotatorGoto(bool& bComplete) const
{
    bComplete = false;

    if (!m_bLinked)
        return ERR_COMMNOLINK;

    if (!m_bDoingGoto) {
        bComplete = true;
        return SB_OK;
    }

    X2CAARotator* pMe = const_cast<X2CAARotator*>(this);

    CAARotator::State s;
    int rc = pMe->m_dev.getState(s);
    if (rc != CAART_OK) {
        logDebug(1, "isCompleteRotatorGoto: getState failed (rc=%d)", rc);
        return ERR_CMDFAILED;
    }

    pMe->m_dCurrentPosition = s.angle;
    bComplete = !s.isMoving;
    logDebug(3, "isCompleteRotatorGoto: angle=%.4f  moving=%d",
             s.angle, (int)s.isMoving);
    return SB_OK;
}

int X2CAARotator::endRotatorGoto()
{
    m_bDoingGoto = false;
    logDebug(2, "endRotatorGoto: pos=%.4f", m_dCurrentPosition);
    return SB_OK;
}

// ── Settings persistence ──────────────────────────────────────────────────────

void X2CAARotator::loadSettings()
{
    if (!m_pIniUtil)
        return;

    char buf[256];

    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_SERIAL,
                           "", m_szStoredSerial, (int)sizeof(m_szStoredSerial));

    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_PATH,
                           "", buf, (int)sizeof(buf));
    snprintf(m_szStoredPath, sizeof(m_szStoredPath), "%s", buf);

    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_BEEP, "1", buf, 8);
    m_bBeep = (buf[0] == '1');

    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_REVERSE, "0", buf, 8);
    m_bReverse = (buf[0] == '1');

    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_MAX_ANGLE, "360", buf, 16);
    m_dMaxAngle = atof(buf);
    if (m_dMaxAngle < 1.0 || m_dMaxAngle > 360.0)
        m_dMaxAngle = 360.0;

    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_DEBUG_LEVEL, "0", buf, 8);
    m_nDebugLevel = atoi(buf);
}

void X2CAARotator::saveSettings()
{
    if (!m_pIniUtil)
        return;

    m_pIniUtil->writeString(X2CAART_INI_SECTION, KEY_SERIAL, m_szStoredSerial);
    m_pIniUtil->writeString(X2CAART_INI_SECTION, KEY_PATH,   m_szStoredPath);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", m_bBeep    ? 1 : 0);
    m_pIniUtil->writeString(X2CAART_INI_SECTION, KEY_BEEP, buf);

    snprintf(buf, sizeof(buf), "%d", m_bReverse ? 1 : 0);
    m_pIniUtil->writeString(X2CAART_INI_SECTION, KEY_REVERSE, buf);

    snprintf(buf, sizeof(buf), "%.0f", m_dMaxAngle);
    m_pIniUtil->writeString(X2CAART_INI_SECTION, KEY_MAX_ANGLE, buf);

    snprintf(buf, sizeof(buf), "%d", m_nDebugLevel);
    m_pIniUtil->writeString(X2CAART_INI_SECTION, KEY_DEBUG_LEVEL, buf);
}

void X2CAARotator::applySettingsToDevice()
{
    if (!m_bLinked)
        return;
    LOCK_IO;
    m_dev.setBeep(m_bBeep);
    m_dev.setReverse(m_bReverse);
    m_dev.setMaxDegree(m_dMaxAngle);
}

// ── Settings dialog ───────────────────────────────────────────────────────────

int X2CAARotator::execModalSettingsDialog()
{
    int nErr = SB_OK;

    X2ModalUIUtil uiutil(this, m_pTheSkyX);
    X2GUIInterface*         ui = uiutil.X2UI();
    X2GUIExchangeInterface* dx = NULL;
    bool bPressedOK = false;

    if (!ui)
        return ERR_POINTER;

    nErr = ui->loadUserInterface("x2caarotator.ui", deviceType(), m_nInstanceIndex);
    if (nErr)
        return nErr;

    dx = uiutil.X2DX();
    if (!dx)
        return ERR_POINTER;

    // ── Populate device selector ──────────────────────────────────────────────
    CAADeviceEntry devs[X2CAART_MAX_DEVICES];
    int nDevs = CAARotator::enumerateDevices(devs, X2CAART_MAX_DEVICES);

    // Always add a "(auto-detect)" option first
    dx->comboBoxAppendString("comboDevice", "(auto-detect)");
    int selectedIdx = 0;  // default to auto-detect

    for (int i = 0; i < nDevs; i++) {
        char label[320];
        makeDeviceLabel(devs[i], label, (int)sizeof(label));
        dx->comboBoxAppendString("comboDevice", label);

        // Pre-select if serial matches stored preference
        if (m_szStoredSerial[0] && strcmp(devs[i].serial, m_szStoredSerial) == 0)
            selectedIdx = i + 1;
        // Or if path matches and no serial preference
        else if (!m_szStoredSerial[0] && m_szStoredPath[0] &&
                 strcmp(devs[i].path, m_szStoredPath) == 0)
            selectedIdx = i + 1;
    }
    dx->setCurrentIndex("comboDevice", selectedIdx);

    // ── Populate info fields ──────────────────────────────────────────────────
    if (m_bLinked) {
        dx->setPropertyString("lblDevPathVal",  "text", m_szDevPath[0]  ? m_szDevPath  : "(auto)");
        dx->setPropertyString("lblFirmwareVal", "text", m_szFirmware[0] ? m_szFirmware : "—");
        dx->setPropertyString("lblSerialVal",   "text", m_szSerial[0]   ? m_szSerial   : "—");
        dx->setPropertyString("lblTypeVal",     "text", m_szType[0]     ? m_szType     : "—");
    } else {
        dx->setPropertyString("lblDevPathVal",  "text", "(not connected)");
        dx->setPropertyString("lblFirmwareVal", "text", "—");
        dx->setPropertyString("lblSerialVal",   "text", "—");
        dx->setPropertyString("lblTypeVal",     "text", "—");
    }

    // ── Populate settings ─────────────────────────────────────────────────────
    dx->setChecked("chkBeep",    m_bBeep);
    dx->setChecked("chkReverse", m_bReverse);
    dx->setCurrentIndex("comboMaxAngle", (m_dMaxAngle <= 180.5) ? 0 : 1);
    dx->setCurrentIndex("comboDebug", m_nDebugLevel);

    nErr = ui->exec(bPressedOK);
    if (nErr)
        return nErr;

    if (bPressedOK) {
        // ── Read device selection ─────────────────────────────────────────────
        int devIdx = dx->currentIndex("comboDevice");
        if (devIdx == 0) {
            // Auto-detect: clear stored preference
            m_szStoredSerial[0] = '\0';
            m_szStoredPath[0]   = '\0';
        } else if (devIdx - 1 < nDevs) {
            const CAADeviceEntry& chosen = devs[devIdx - 1];
            snprintf(m_szStoredSerial, sizeof(m_szStoredSerial), "%s", chosen.serial);
            snprintf(m_szStoredPath,   sizeof(m_szStoredPath),   "%s", chosen.path);
        }

        // ── Read other settings ───────────────────────────────────────────────
        m_bBeep       = dx->isChecked("chkBeep");
        m_bReverse    = dx->isChecked("chkReverse");
        m_dMaxAngle   = (dx->currentIndex("comboMaxAngle") == 0) ? 180.0 : 360.0;
        m_nDebugLevel = dx->currentIndex("comboDebug");

        saveSettings();
        applySettingsToDevice();

        logDebug(2,
                 "settings saved: serial=%s path=%s beep=%d reverse=%d max=%.0f debug=%d",
                 m_szStoredSerial, m_szStoredPath,
                 (int)m_bBeep, (int)m_bReverse, m_dMaxAngle, m_nDebugLevel);
    }

    return SB_OK;
}

// ── X2GUIEventInterface ───────────────────────────────────────────────────────

void X2CAARotator::uiEvent(X2GUIExchangeInterface* /*uiex*/, const char* /*pszEvent*/)
{
    // No dynamic UI events in this dialog — all settings are read on OK.
}

// ── Logging ───────────────────────────────────────────────────────────────────

void X2CAARotator::logDebug(int minLevel, const char* fmt, ...) const
{
    if (m_nDebugLevel < minLevel)
        return;
    if (!m_pLogger)
        return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char msg[560];
    snprintf(msg, sizeof(msg), "[X2CAARotator] %s", buf);
    m_pLogger->out(msg);
}

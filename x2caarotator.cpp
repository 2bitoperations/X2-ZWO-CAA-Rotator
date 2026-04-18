/*
 * x2caarotator.cpp — X2 Rotator plugin for the ZWO CAA Camera Angle Adjuster
 */

#include "x2caarotator.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define LOCK_IO  X2MutexLocker _lock(m_pIOMutex)

// INI keys (all scoped to X2CAART_INI_SECTION + instance index suffix)
#define KEY_DEV_PATH    "DevicePath"
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
    m_szFirmware[0] = '\0';
    m_szType[0]     = '\0';
    m_szSerial[0]   = '\0';
    m_szDevPath[0]  = '\0';

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
    if (m_szFirmware[0])
        str = m_szFirmware;
    else
        str = "unknown";
}

void X2CAARotator::deviceInfoModel(BasicStringInterface& str)
{
    if (m_szType[0])
        str = m_szType;
    else
        str = "ZWO CAA";
}

// ── LinkInterface ─────────────────────────────────────────────────────────────

int X2CAARotator::establishLink()
{
    LOCK_IO;

    if (m_bLinked)
        return SB_OK;

    // Determine the hidraw path to open
    char path[64];
    if (m_szDevPath[0]) {
        snprintf(path, sizeof(path), "%s", m_szDevPath);
    } else {
        int rc = CAARotator::findDevice(path, (int)sizeof(path));
        if (rc != CAART_OK) {
            logDebug(1, "establishLink: no ZWO CAA device found (rc=%d)", rc);
            return ERR_COMMNOLINK;
        }
    }

    int rc = m_dev.open(path);
    if (rc != CAART_OK) {
        logDebug(1, "establishLink: open(%s) failed (rc=%d)", path, rc);
        return ERR_COMMOPENING;
    }

    // Read device information (flushes buffer internally)
    char fw[16], type[16], serial[24];
    rc = m_dev.getInfo(fw, type, serial);
    if (rc == CAART_OK) {
        snprintf(m_szFirmware, sizeof(m_szFirmware), "%s", fw);
        snprintf(m_szType,     sizeof(m_szType),     "%s", type);
        snprintf(m_szSerial,   sizeof(m_szSerial),   "%s", serial);
    }

    // Read live state (position, beep, reverse, maxAngle)
    CAARotator::State s;
    rc = m_dev.getState(s);
    if (rc == CAART_OK) {
        m_dCurrentPosition = s.angle;
        m_bBeep            = s.beep;
        m_bReverse         = s.reverse;
        m_dMaxAngle        = s.maxAngle;
    }

    snprintf(m_szDevPath, sizeof(m_szDevPath), "%s", path);
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

    // Cast away const so we can update state (matches example plugin pattern)
    X2CAARotator* pMe = const_cast<X2CAARotator*>(this);

    CAARotator::State s;
    int rc = pMe->m_dev.getState(s);
    if (rc != CAART_OK) {
        logDebug(1, "isCompleteRotatorGoto: getState failed (rc=%d)", rc);
        return ERR_CMDFAILED;
    }

    pMe->m_dCurrentPosition = s.angle;
    bComplete = !s.isMoving;
    logDebug(3, "isCompleteRotatorGoto: angle=%.4f  moving=%d", s.angle, (int)s.isMoving);
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

    char defPath[64] = "";
    char pathBuf[64];
    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_DEV_PATH,
                           defPath, pathBuf, (int)sizeof(pathBuf));
    snprintf(m_szDevPath, sizeof(m_szDevPath), "%s", pathBuf);

    char beepBuf[8] = "1";
    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_BEEP,
                           "1", beepBuf, (int)sizeof(beepBuf));
    m_bBeep = (beepBuf[0] == '1');

    char revBuf[8] = "0";
    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_REVERSE,
                           "0", revBuf, (int)sizeof(revBuf));
    m_bReverse = (revBuf[0] == '1');

    char maxBuf[16] = "360";
    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_MAX_ANGLE,
                           "360", maxBuf, (int)sizeof(maxBuf));
    m_dMaxAngle = atof(maxBuf);
    if (m_dMaxAngle < 1.0 || m_dMaxAngle > 360.0)
        m_dMaxAngle = 360.0;

    char dbgBuf[8] = "0";
    m_pIniUtil->readString(X2CAART_INI_SECTION, KEY_DEBUG_LEVEL,
                           "0", dbgBuf, (int)sizeof(dbgBuf));
    m_nDebugLevel = atoi(dbgBuf);
}

void X2CAARotator::saveSettings()
{
    if (!m_pIniUtil)
        return;

    m_pIniUtil->writeString(X2CAART_INI_SECTION, KEY_DEV_PATH, m_szDevPath);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", m_bBeep ? 1 : 0);
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

    // Populate read-only info fields
    dx->setPropertyString("lblDevPathVal",  "text", m_szDevPath[0] ? m_szDevPath : "(auto)");
    dx->setPropertyString("lblFirmwareVal", "text", m_szFirmware[0] ? m_szFirmware : "—");
    dx->setPropertyString("lblSerialVal",   "text", m_szSerial[0]   ? m_szSerial   : "—");
    dx->setPropertyString("lblTypeVal",     "text", m_szType[0]     ? m_szType     : "—");

    // Populate editable settings
    dx->setChecked("chkBeep",    m_bBeep);
    dx->setChecked("chkReverse", m_bReverse);

    // Max angle combo: 0 = 180°, 1 = 360°
    dx->setCurrentIndex("comboMaxAngle",
                        (m_dMaxAngle <= 180.5) ? 0 : 1);

    // Debug level combo: index = level (0–3)
    dx->setCurrentIndex("comboDebug", m_nDebugLevel);

    nErr = ui->exec(bPressedOK);
    if (nErr)
        return nErr;

    if (bPressedOK) {
        bool beepNew    = dx->isChecked("chkBeep");
        bool reverseNew = dx->isChecked("chkReverse");
        int  maxIdx     = dx->currentIndex("comboMaxAngle");
        int  dbgIdx     = dx->currentIndex("comboDebug");

        double maxNew   = (maxIdx == 0) ? 180.0 : 360.0;

        m_bBeep       = beepNew;
        m_bReverse    = reverseNew;
        m_dMaxAngle   = maxNew;
        m_nDebugLevel = dbgIdx;

        saveSettings();
        applySettingsToDevice();
        logDebug(2, "settings saved: beep=%d reverse=%d max=%.0f debug=%d",
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

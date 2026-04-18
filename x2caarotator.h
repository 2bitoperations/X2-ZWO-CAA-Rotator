#pragma once

/*
 * x2caarotator.h — X2 Rotator plugin for the ZWO CAA Camera Angle Adjuster
 *
 * Implements RotatorDriverInterface using direct HID I/O (no serial port).
 * ModalSettingsDialogInterface / X2GUIEventInterface provide the settings dialog.
 */

#include "licensedinterfaces/sberrorx.h"
#include "licensedinterfaces/basicstringinterface.h"
#include "licensedinterfaces/rotatordriverinterface.h"
#include "licensedinterfaces/serxinterface.h"
#include "licensedinterfaces/theskyxfacadefordriversinterface.h"
#include "licensedinterfaces/sleeperinterface.h"
#include "licensedinterfaces/basiciniutilinterface.h"
#include "licensedinterfaces/loggerinterface.h"
#include "licensedinterfaces/mutexinterface.h"
#include "licensedinterfaces/tickcountinterface.h"
#include "licensedinterfaces/modalsettingsdialoginterface.h"
#include "licensedinterfaces/x2guiinterface.h"

#include "caarotator.h"
#include "version.h"

#include <string>

#define X2CAART_INI_SECTION  "CAARotator"

class X2CAARotator : public RotatorDriverInterface
                   , public ModalSettingsDialogInterface
                   , public X2GUIEventInterface
{
public:
    X2CAARotator(
        const char*                         pszDisplayName,
        const int&                          nInstanceIndex,
        SerXInterface*                      pSerXIn,
        TheSkyXFacadeForDriversInterface*   pTheSkyXIn,
        SleeperInterface*                   pSleeperIn,
        BasicIniUtilInterface*              pIniUtilIn,
        LoggerInterface*                    pLoggerIn,
        MutexInterface*                     pIOMutexIn,
        TickCountInterface*                 pTickCountIn);

    virtual ~X2CAARotator();

    // ── DriverRootInterface ──────────────────────────────────────────────────
    virtual DeviceType  deviceType()                    { return DriverRootInterface::DT_ROTATOR; }
    virtual int         queryAbstraction(const char* pszName, void** ppVal);

    // ── DriverInfoInterface ──────────────────────────────────────────────────
    virtual void        driverInfoDetailedInfo(BasicStringInterface& str) const;
    virtual double      driverInfoVersion() const       { return PLUGIN_VERSION_DOUBLE; }

    // ── HardwareInfoInterface ────────────────────────────────────────────────
    virtual void        deviceInfoNameShort(BasicStringInterface& str) const;
    virtual void        deviceInfoNameLong(BasicStringInterface& str) const;
    virtual void        deviceInfoDetailedDescription(BasicStringInterface& str) const;
    virtual void        deviceInfoFirmwareVersion(BasicStringInterface& str);
    virtual void        deviceInfoModel(BasicStringInterface& str);

    // ── LinkInterface ────────────────────────────────────────────────────────
    virtual int         establishLink();
    virtual int         terminateLink();
    virtual bool        isLinked() const                { return m_bLinked; }
    virtual bool        isEstablishLinkAbortable() const { return false; }

    // ── RotatorDriverInterface ───────────────────────────────────────────────
    virtual int         position(double& dPosition);
    virtual int         abort();
    virtual int         startRotatorGoto(const double& dTargetPosition);
    virtual int         isCompleteRotatorGoto(bool& bComplete) const;
    virtual int         endRotatorGoto();

    // ── ModalSettingsDialogInterface ─────────────────────────────────────────
    virtual int         initModalSettingsDialog()       { return SB_OK; }
    virtual int         execModalSettingsDialog();

    // ── X2GUIEventInterface ──────────────────────────────────────────────────
    virtual void        uiEvent(X2GUIExchangeInterface* uiex, const char* pszEvent);

private:
    // TheSkyX-provided interfaces (not owned — do NOT delete)
    SerXInterface*                      m_pSerX;
    TheSkyXFacadeForDriversInterface*   m_pTheSkyX;
    SleeperInterface*                   m_pSleeper;
    BasicIniUtilInterface*              m_pIniUtil;
    LoggerInterface*                    m_pLogger;
    MutexInterface*                     m_pIOMutex;
    TickCountInterface*                 m_pTickCount;

    int     m_nInstanceIndex;
    bool    m_bLinked;
    int     m_nDebugLevel;   // 0=Off 1=Errors 2=Cmds 3=Full

    // Low-level HID driver
    CAARotator  m_dev;

    // Cached device info (populated in establishLink)
    char    m_szFirmware[16];
    char    m_szType[16];
    char    m_szSerial[24];
    char    m_szDevPath[64];

    // Motion state
    bool    m_bDoingGoto;
    double  m_dTargetPosition;
    double  m_dCurrentPosition;

    // Persisted settings
    bool    m_bBeep;
    bool    m_bReverse;
    double  m_dMaxAngle;   // 180.0 or 360.0

    // Settings helpers
    void    loadSettings();
    void    saveSettings();
    void    applySettingsToDevice();

    // Logging: only writes when m_nDebugLevel >= minLevel
    void    logDebug(int minLevel, const char* fmt, ...) const;
};

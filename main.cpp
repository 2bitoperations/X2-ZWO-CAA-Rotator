#include "main.h"
#include "x2caarotator.h"

extern "C" PlugInExport int sbPlugInName2(BasicStringInterface& str)
{
    str = "ZWO CAA Rotator";
    return SB_OK;
}

extern "C" PlugInExport int sbPlugInFactory2(
    const char*                             pszDisplayName,
    const int&                              nInstanceIndex,
    SerXInterface*                          pSerXIn,
    TheSkyXFacadeForDriversInterface*       pTheSkyXIn,
    SleeperInterface*                       pSleeperIn,
    BasicIniUtilInterface*                  pIniUtilIn,
    LoggerInterface*                        pLoggerIn,
    MutexInterface*                         pIOMutexIn,
    TickCountInterface*                     pTickCountIn,
    void**                                  ppObjectOut)
{
    *ppObjectOut = NULL;

    X2CAARotator* pObj = new X2CAARotator(
        pszDisplayName,
        nInstanceIndex,
        pSerXIn,
        pTheSkyXIn,
        pSleeperIn,
        pIniUtilIn,
        pLoggerIn,
        pIOMutexIn,
        pTickCountIn);

    if (pObj != NULL)
        *ppObjectOut = dynamic_cast<RotatorDriverInterface*>(pObj);

    return SB_OK;
}

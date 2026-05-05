/**
 * @file   checkerboard.cpp
 * @brief  plug-in-checkerboard ブリッジ実装（CSPBridge アライン版）
 * @author CSPBridgeGimp
 * @date   2026-05-04
 *
 * GIMP の checkerboard プラグインを CSP からブリッジ呼び出しする。
 * パラメーター:
 *   - psychobilly (gboolean) : サイケな配色モード
 *   - check-size  (gint)     : チェック 1 マスのサイズ (px)
 */

#include "plugin_iface.h"

namespace
{
constexpr int ItemKeyPsychobilly = 1;
constexpr int ItemKeyCheckSize   = 2;
}

PluginInfo GetPluginInfo()
{
    return {
        .exeName     = "checkerboard",
        .procName    = "plug-in-checkerboard",
        .displayName = "Checkerboard",
        .canPreview  = true,
    };
}

void SetupProperty(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInStringService*     strSvc,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  /*propSvc2*/)
{
    // Psychobilly (Boolean)
    auto psyLabel = CreateAsciiString(strSvc, "Psychobilly");
    propSvc->addItemProc(propObj, ItemKeyPsychobilly,
        kTriglavPlugInPropertyValueTypeBoolean,
        kTriglavPlugInPropertyValueKindDefault,
        kTriglavPlugInPropertyInputKindDefault,
        psyLabel, '\0');
    propSvc->setBooleanValueProc(propObj, ItemKeyPsychobilly, kTriglavPlugInBoolFalse);
    strSvc->releaseProc(psyLabel);

    // Check Size (Integer slider)
    auto szLabel = CreateAsciiString(strSvc, "Check Size");
    propSvc->addItemProc(propObj, ItemKeyCheckSize,
        kTriglavPlugInPropertyValueTypeInteger,
        kTriglavPlugInPropertyValueKindDefault,
        kTriglavPlugInPropertyInputKindDefault,
        szLabel, '\0');
    propSvc->setIntegerValueProc       (propObj, ItemKeyCheckSize, 10);
    propSvc->setIntegerDefaultValueProc(propObj, ItemKeyCheckSize, 10);
    propSvc->setIntegerMinValueProc    (propObj, ItemKeyCheckSize, 1);
    propSvc->setIntegerMaxValueProc    (propObj, ItemKeyCheckSize, 200);
    strSvc->releaseProc(szLabel);
}

FilterParams BuildFilterParams(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  /*propSvc2*/)
{
    TriglavPlugInBool psy = kTriglavPlugInBoolFalse;
    TriglavPlugInInt  sz  = 10;

    if (propObj && propSvc)
    {
        propSvc->getBooleanValueProc(&psy, propObj, ItemKeyPsychobilly);
        propSvc->getIntegerValueProc(&sz,  propObj, ItemKeyCheckSize);
    }

    FilterParams params;
    params.procedureName = "plug-in-checkerboard";
    params.args = {
        GpParam{GpParamType::Int, "gboolean", "",
                psy != kTriglavPlugInBoolFalse ? 1 : 0, 0.0},  // psychobilly
        GpParam{GpParamType::Int, "gint",     "",
                static_cast<int>(sz), 0.0},                    // check-size
    };
    return params;
}

TriglavPlugInInt OnPropertyChanged(
    TriglavPlugInPropertyObject     /*propObj*/,
    TriglavPlugInInt                /*itemKey*/,
    TriglavPlugInInt                notify,
    TriglavPlugInPropertyService*   /*propSvc*/,
    TriglavPlugInPropertyService2*  /*propSvc2*/)
{
    // ValueChanged のとき Modify を返してプレビューの再計算をトリガーする。
    return (notify == kTriglavPlugInPropertyCallBackNotifyValueChanged)
        ? kTriglavPlugInPropertyCallBackResultModify
        : kTriglavPlugInPropertyCallBackResultNoModify;
}

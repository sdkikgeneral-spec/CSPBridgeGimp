/**
 * @file   despeckle.cpp
 * @brief  plug-in-despeckle ブリッジ実装（CSPBridge アライン版）
 * @author CSPBridgeGimp
 * @date   2026-05-06
 *
 * GIMP の despeckle プラグインを CSP からブリッジ呼び出しする。
 * パラメーター:
 *   - radius (gint)      : メディアンフィルター半径 (1–30)
 *   - type   (GimpChoice): フィルタータイプ（固定: "adaptive"）
 *   - black  (gint)      : 黒しきい値（固定: 7）
 *   - white  (gint)      : 白しきい値（固定: 248）
 */

#include "plugin_iface.h"

namespace
{
constexpr int ItemKeyRadius = 1;
}

PluginInfo GetPluginInfo()
{
    return {
        .exeName     = "despeckle",
        .procName    = "plug-in-despeckle",
        .displayName = "Despeckle",
        .canPreview  = true,
    };
}

void SetupProperty(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInStringService*     strSvc,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  /*propSvc2*/)
{
    if (!propObj || !strSvc || !propSvc) return;

    // Radius (Integer slider)
    auto radLabel = CreateAsciiString(strSvc, "Radius");
    propSvc->addItemProc(propObj, ItemKeyRadius,
        kTriglavPlugInPropertyValueTypeInteger,
        kTriglavPlugInPropertyValueKindDefault,
        kTriglavPlugInPropertyInputKindDefault,
        radLabel, '\0');
    propSvc->setIntegerValueProc       (propObj, ItemKeyRadius, 3);
    propSvc->setIntegerDefaultValueProc(propObj, ItemKeyRadius, 3);
    propSvc->setIntegerMinValueProc    (propObj, ItemKeyRadius, 1);
    propSvc->setIntegerMaxValueProc    (propObj, ItemKeyRadius, 30);
    if (radLabel) strSvc->releaseProc(radLabel);
}

FilterParams BuildFilterParams(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  /*propSvc2*/)
{
    TriglavPlugInInt radius = 3;

    if (propObj && propSvc)
    {
        propSvc->getIntegerValueProc(&radius, propObj, ItemKeyRadius);
    }

    FilterParams params;
    params.procedureName = "plug-in-despeckle";
    params.args = {
        GpParam{GpParamType::Int,    "gint",       "",         static_cast<int>(radius), 0.0},  // radius
        GpParam{GpParamType::String, "gchararray", "adaptive", 0,                        0.0},  // type (固定)
        GpParam{GpParamType::Int,    "gint",       "",         7,                        0.0},  // black (固定)
        GpParam{GpParamType::Int,    "gint",       "",         248,                      0.0},  // white (固定)
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

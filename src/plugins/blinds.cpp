/**
 * @file   blinds.cpp
 * @brief  plug-in-blinds ブリッジ実装（CSPBridge アライン版）
 * @author CSPBridgeGimp
 * @date   2026-05-06
 *
 * GIMP の blinds プラグインを CSP からブリッジ呼び出しする。
 * パラメーター:
 *   - angle-displacement (gint)      : ブラインドの角度 (0–90)
 *   - num-segments       (gint)      : セグメント数 (1–64)
 *   - orientation        (GimpChoice): 向き（固定: "horizontal"）
 *   - bg-transparent     (gboolean)  : 背景透過（固定: FALSE）
 */

#include "plugin_iface.h"

namespace
{
constexpr int ItemKeyAngle    = 1;
constexpr int ItemKeySegments = 2;
}

PluginInfo GetPluginInfo()
{
    return {
        .exeName     = "blinds",
        .procName    = "plug-in-blinds",
        .displayName = "Blinds",
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

    // Angle (Integer slider)
    auto angleLabel = CreateAsciiString(strSvc, "Angle");
    propSvc->addItemProc(propObj, ItemKeyAngle,
        kTriglavPlugInPropertyValueTypeInteger,
        kTriglavPlugInPropertyValueKindDefault,
        kTriglavPlugInPropertyInputKindDefault,
        angleLabel, '\0');
    propSvc->setIntegerValueProc       (propObj, ItemKeyAngle, 30);
    propSvc->setIntegerDefaultValueProc(propObj, ItemKeyAngle, 30);
    propSvc->setIntegerMinValueProc    (propObj, ItemKeyAngle, 0);
    propSvc->setIntegerMaxValueProc    (propObj, ItemKeyAngle, 90);
    if (angleLabel) strSvc->releaseProc(angleLabel);

    // Segments (Integer slider)
    auto segLabel = CreateAsciiString(strSvc, "Segments");
    propSvc->addItemProc(propObj, ItemKeySegments,
        kTriglavPlugInPropertyValueTypeInteger,
        kTriglavPlugInPropertyValueKindDefault,
        kTriglavPlugInPropertyInputKindDefault,
        segLabel, '\0');
    propSvc->setIntegerValueProc       (propObj, ItemKeySegments, 3);
    propSvc->setIntegerDefaultValueProc(propObj, ItemKeySegments, 3);
    propSvc->setIntegerMinValueProc    (propObj, ItemKeySegments, 1);
    propSvc->setIntegerMaxValueProc    (propObj, ItemKeySegments, 64);
    if (segLabel) strSvc->releaseProc(segLabel);
}

FilterParams BuildFilterParams(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  /*propSvc2*/)
{
    TriglavPlugInInt angle    = 30;
    TriglavPlugInInt segments = 3;

    if (propObj && propSvc)
    {
        propSvc->getIntegerValueProc(&angle,    propObj, ItemKeyAngle);
        propSvc->getIntegerValueProc(&segments, propObj, ItemKeySegments);
    }

    FilterParams params;
    params.procedureName = "plug-in-blinds";
    params.args = {
        GpParam{GpParamType::Int,    "gint",       "",           static_cast<int>(angle),    0.0},  // angle-displacement
        GpParam{GpParamType::Int,    "gint",       "",           static_cast<int>(segments), 0.0},  // num-segments
        GpParam{GpParamType::String, "gchararray", "horizontal", 0,                          0.0},  // orientation (固定)
        GpParam{GpParamType::Int,    "gboolean",   "",           0,                          0.0},  // bg-transparent (固定 FALSE)
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

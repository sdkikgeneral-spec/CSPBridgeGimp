/**
 * @file   checkerboard.cpp
 * @brief  plug-in-checkerboard ブリッジ実装
 * @author CSPBridgeGimp
 * @date   2026-05-03
 */
#include "plugin_iface.h"

PluginInfo GetPluginInfo()
{
    return {"checkerboard", "plug-in-checkerboard"};
}

std::vector<PropItemDef> GetProperties()
{
    return {PropItemDef{1, "Check Size", 10, 1, 200}};
}

FilterParams BuildFilterParams(
    TriglavPlugInPropertyObject   propObj,
    TriglavPlugInPropertyService* propSvc)
{
    TriglavPlugInInt checkSize = 10;
    if (propObj && propSvc)
        propSvc->getIntegerValueProc(&checkSize, propObj, 1);

    FilterParams params;
    params.procedureName = "plug-in-checkerboard";
    params.args = {
        GpParam{GpParamType::Int, "gboolean", "", 0,              0.0},  // psychobilly
        GpParam{GpParamType::Int, "gint",     "", (int)checkSize, 0.0},  // check-size
    };
    return params;
}

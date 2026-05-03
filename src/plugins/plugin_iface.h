/**
 * @file   plugin_iface.h
 * @brief  プラグイン層インターフェース定義
 * @author CSPBridgeGimp
 * @date   2026-05-03
 */
#pragma once
#include <string>
#include <vector>
#include "../ipc/wire_io.h"
#include "TriglavPlugInService.h"

/// @brief GIMP プラグイン実行情報
struct PluginInfo
{
    std::string exeName;   ///< EXE ファイル名（拡張子なし。例: "checkerboard"）
    std::string procName;  ///< プロシージャ名（例: "plug-in-checkerboard"）
};

/// @brief CSP プロパティ UI 整数スライダー定義
struct PropItemDef
{
    int         key;         ///< 1-based キー（addItemProc に渡す値）
    std::string label;       ///< UI 表示ラベル
    int         defaultVal;  ///< デフォルト値
    int         minVal;      ///< 最小値
    int         maxVal;      ///< 最大値
};

/** @brief プラグインの EXE 名・プロシージャ名を返す */
PluginInfo GetPluginInfo();

/**
 * @brief  CSP プロパティ UI に表示するスライダー定義一覧を返す
 * @return PropItemDef のリスト。空の場合は UI なし
 */
std::vector<PropItemDef> GetProperties();

/**
 * @brief  CSP プロパティ現在値から GIMP FilterParams を組み立てる
 * @param  propObj  FilterRun 時に TriglavPlugInFilterRunGetProperty で取得した propObj
 * @param  propSvc  PropertyService ポインタ（getIntegerValueProc で値を読む）
 * @return GIMP PluginSession に渡す FilterParams
 */
FilterParams BuildFilterParams(
    TriglavPlugInPropertyObject   propObj,
    TriglavPlugInPropertyService* propSvc);

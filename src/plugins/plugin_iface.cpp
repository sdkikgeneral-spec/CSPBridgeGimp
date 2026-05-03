/**
 * @file   plugin_iface.cpp
 * @brief  プラグイン層インターフェースの共通ヘルパー実装
 * @author CSPBridgeGimp
 * @date   2026-05-04
 *
 * 各プラグインから利用される共通ユーティリティ関数を提供する。
 * 現状は `CreateAsciiString` のみ。SDK 直接呼び出しの定型を 1 か所に集約。
 */

#include "plugin_iface.h"

TriglavPlugInStringObject CreateAsciiString(
    TriglavPlugInStringService* strSvc, const std::string& text)
{
    if (strSvc == nullptr)
        return nullptr;

    TriglavPlugInStringObject obj = nullptr;
    strSvc->createWithAsciiStringProc(
        &obj, text.c_str(),
        static_cast<TriglavPlugInInt>(text.size()));
    return obj;
}

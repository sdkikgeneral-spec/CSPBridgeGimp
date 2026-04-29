/**
 * @file   plugin_entry.h
 * @brief  CSP フィルタープラグイン エントリポイント定義
 *
 * TriglavPluginCall は Clip Studio Paint がフィルタープラグイン DLL/dylib を
 * ロードした直後から呼び出す唯一のエクスポート関数。
 * セレクター値に応じてモジュール初期化・フィルター初期化・フィルター実行を
 * それぞれ HandleModuleInitialize / HandleFilterInitialize / HandleFilterRun に
 * 委譲する。
 *
 * このヘッダーは他モジュールからインクルードされることを想定していない。
 * TriglavPlugInMain.h が TriglavPluginCall の extern "C" 宣言を提供するため、
 * 重複宣言は行わない。
 *
 * @note  GIMP_PLUGIN_ID マクロはビルド時に meson.build が
 *        `-DGIMP_PLUGIN_ID="plug-in-gauss"` 形式で定義する。
 *        未定義の場合は #error でビルドを停止する（本ヘッダーをインクルードしたファイル）。
 *
 * @author CSPBridgeGimp
 * @date   2026-04-29
 */
#pragma once

// GIMP_PLUGIN_ID は meson.build が -DGIMP_PLUGIN_ID="plug-in-gauss" 形式で定義する。
// plugin_entry.cpp は GIMP_PLUGIN_ID 未定義でも core_lib としてビルド可能（""フォールバック）。
// 実際の DLL ターゲットでは必ず定義されること。

// ---------------------------------------------------------------------------
// プラットフォーム別 DLL ディレクトリ取得ヘルパーの前方宣言
// ---------------------------------------------------------------------------

#include <string>

/**
 * @brief  この DLL / dylib が配置されているディレクトリのパスを返す
 *
 * Windows: DllMain で保存した g_hModule を使い GetModuleFileNameW で取得。
 * Mac:     TriglavPluginCall アドレスを dladdr に渡して dli_fname からディレクトリ部分を切り出す。
 *
 * @return DLL/dylib ディレクトリの絶対パス（末尾区切り文字なし）
 */
std::string GetModuleDir();

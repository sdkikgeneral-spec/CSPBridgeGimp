/**
 * @file   config.h
 * @brief  JSON 設定ファイルの読み込みと Bridge 設定の管理
 * @author CSPBridgeGimp
 * @date   2026-04-27
 */

#pragma once

#include <string>
#include <vector>

/**
 * @brief Bridge 設定を保持する構造体
 */
struct BridgeConfig
{
    std::vector<std::string> pluginSearchPaths; ///< GIMP プラグイン EXE の検索パス一覧
    std::string gimpLibDir;                     ///< libgimpwire / libgimpbase のディレクトリ
    std::string cspPluginOutputDir;             ///< CSP プラグインのインストール先
};

/**
 * @brief  パス文字列中のプレースホルダーを展開して返す
 *
 * Windows では `%FOO%` 形式を `ExpandEnvironmentStringsA` で展開する。
 * Mac では `{HOME}` を `getenv("HOME")` の値で置換する。
 *
 * @param  path  プレースホルダーを含む可能性があるパス文字列
 * @return 展開済みのパス文字列
 */
std::string ExpandPlaceholders(const std::string& path);

/**
 * @brief  設定ファイルを読み込み、プレースホルダーを展開して返す
 *
 * `configPath` の JSON ファイルを読み込み、実行中のプラットフォームに対応する
 * セクション（`"windows"` または `"mac"`）のみを使用する。
 * ファイルが存在しない場合、または JSON パースに失敗した場合は例外を投げず、
 * プラットフォーム固有のデフォルト値を返す。
 *
 * @param  configPath  bridge_config.json のファイルパス
 * @return 展開済みの BridgeConfig。ファイル不在時はデフォルト値
 */
BridgeConfig LoadConfig(const std::string& configPath);

/**
 * @brief  設定の検索パスから指定プラグイン EXE を探す
 *
 * `cfg.pluginSearchPaths` を先頭から順に走査し、各ディレクトリで
 * `pluginName.exe`（Windows）または `pluginName`（Mac）を探す。
 * 最初に見つかったファイルのフルパスを返す。
 *
 * @param  cfg         LoadConfig() で取得した設定
 * @param  pluginName  プラグイン名（拡張子なし）
 * @return 見つかった EXE のフルパス。見つからない場合は空文字列
 */
std::string FindPluginExe(const BridgeConfig& cfg, const std::string& pluginName);

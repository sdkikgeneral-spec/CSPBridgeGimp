/**
 * @file   config.cpp
 * @brief  JSON 設定ファイルの読み込みと Bridge 設定の管理
 * @author CSPBridgeGimp
 * @date   2026-04-27
 */

#include "config.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

/// プラットフォームに対応する JSON セクション名を返す
constexpr const char* PlatformKey()
{
#ifdef _WIN32
    return "windows";
#else
    return "mac";
#endif
}

/// プラットフォームのデフォルト pluginSearchPaths を返す
std::vector<std::string> DefaultPluginSearchPaths()
{
#ifdef _WIN32
    return { "%PROGRAMFILES%\\GIMP 3\\lib\\gimp\\3.0\\plug-ins" };
#else
    return { "/Applications/GIMP.app/Contents/Resources/lib/gimp/3.0/plug-ins" };
#endif
}

/// すべての出現箇所を置換するヘルパー（Mac 向け）
#ifndef _WIN32
void ReplaceAll(std::string& str, const std::string& from, const std::string& to)
{
    std::size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos)
    {
        str.replace(pos, from.length(), to);
        pos += to.length();
    }
}
#endif

} // anonymous namespace

std::string ExpandPlaceholders(const std::string& path)
{
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    DWORD result = ExpandEnvironmentStringsA(path.c_str(), buf, static_cast<DWORD>(sizeof(buf)));
    if (result == 0 || result > sizeof(buf))
    {
        return path;
    }
    return buf;
#else
    std::string expanded = path;
    const char* home = getenv("HOME");
    if (home != nullptr)
    {
        ReplaceAll(expanded, "{HOME}", home);
    }
    return expanded;
#endif
}

BridgeConfig LoadConfig(const std::string& configPath)
{
    BridgeConfig cfg;

    // ファイルが存在しない場合はデフォルト値を返す
    if (!std::filesystem::exists(configPath))
    {
        cfg.pluginSearchPaths = DefaultPluginSearchPaths();
        return cfg;
    }

    std::ifstream ifs(configPath);
    if (!ifs.is_open())
    {
        cfg.pluginSearchPaths = DefaultPluginSearchPaths();
        return cfg;
    }

    nlohmann::json root;
    try
    {
        ifs >> root;
    }
    catch (const nlohmann::json::exception&)
    {
        cfg.pluginSearchPaths = DefaultPluginSearchPaths();
        return cfg;
    }

    const char* platformKey = PlatformKey();

    // プラットフォームセクションが存在しない場合はデフォルト値を返す
    if (!root.contains(platformKey) || !root[platformKey].is_object())
    {
        cfg.pluginSearchPaths = DefaultPluginSearchPaths();
        return cfg;
    }

    const nlohmann::json& section = root[platformKey];

    // plugin_search_paths の読み込みと展開
    if (section.contains("plugin_search_paths") && section["plugin_search_paths"].is_array())
    {
        for (const auto& entry : section["plugin_search_paths"])
        {
            if (entry.is_string())
            {
                cfg.pluginSearchPaths.push_back(ExpandPlaceholders(entry.get<std::string>()));
            }
        }
    }
    if (cfg.pluginSearchPaths.empty())
    {
        cfg.pluginSearchPaths = DefaultPluginSearchPaths();
    }

    // gimp_lib_dir の読み込みと展開
    if (section.contains("gimp_lib_dir") && section["gimp_lib_dir"].is_string())
    {
        cfg.gimpLibDir = ExpandPlaceholders(section["gimp_lib_dir"].get<std::string>());
    }

    // csp_plugin_output_dir の読み込みと展開
    if (section.contains("csp_plugin_output_dir") && section["csp_plugin_output_dir"].is_string())
    {
        cfg.cspPluginOutputDir =
            ExpandPlaceholders(section["csp_plugin_output_dir"].get<std::string>());
    }

    return cfg;
}

std::string FindPluginExe(const BridgeConfig& cfg, const std::string& pluginName)
{
#ifdef _WIN32
    const std::string exeName = pluginName + ".exe";
#else
    const std::string exeName = pluginName;
#endif

    for (const auto& searchDir : cfg.pluginSearchPaths)
    {
        std::filesystem::path candidate = std::filesystem::path(searchDir) / exeName;
        if (std::filesystem::exists(candidate))
        {
            return candidate.string();
        }
    }

    return {};
}

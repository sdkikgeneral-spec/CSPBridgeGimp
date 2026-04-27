/**
 * @file   test_config.cpp
 * @brief  src/config/config の単体テスト
 * @author CSPBridgeGimp
 * @date   2026-04-27
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "../src/config/config.h"

// ---------------------------------------------------------------------------
// テストヘルパー
// ---------------------------------------------------------------------------

namespace
{

/// 一時ファイルに内容を書き出し、スコープ終了時に削除するガード
struct TempFile
{
    std::filesystem::path path;

    TempFile(const std::string& filename, const std::string& content)
        : path(std::filesystem::temp_directory_path() / filename)
    {
        std::ofstream ofs(path);
        ofs << content;
    }

    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    std::string Str() const
    {
        return path.string();
    }
};

constexpr const char* VALID_JSON = R"({
  "windows": {
    "plugin_search_paths": [
      "C:/Program Files/GIMP 3/lib/gimp/3.0/plug-ins",
      "%APPDATA%/GIMP/3.0/plug-ins"
    ],
    "gimp_lib_dir": "C:/Program Files/GIMP 3/bin",
    "csp_plugin_output_dir": "%APPDATA%/CELSYSUserData/CELSYS/CLIPStudioModule/PlugIn/PAINT"
  },
  "mac": {
    "plugin_search_paths": [
      "/Applications/GIMP.app/Contents/Resources/lib/gimp/3.0/plug-ins",
      "{HOME}/Library/Application Support/GIMP/3.0/plug-ins"
    ],
    "gimp_lib_dir": "/Applications/GIMP.app/Contents/Resources/lib",
    "csp_plugin_output_dir": "{HOME}/Library/Application Support/CELSYSUserData/CELSYS/CLIPStudioModule/PlugIn/PAINT"
  }
})";

} // anonymous namespace

// ---------------------------------------------------------------------------
// ExpandPlaceholders テスト
// ---------------------------------------------------------------------------

#ifdef _WIN32

TEST_CASE("ExpandPlaceholders: %APPDATA% expands on Windows", "[config]")
{
    std::string path = "%APPDATA%/GIMP/3.0/plug-ins";
    std::string expanded = ExpandPlaceholders(path);

    // プレースホルダーが消えていること
    REQUIRE(expanded.find("%APPDATA%") == std::string::npos);
    // 環境変数が展開されているため空にはならないこと
    REQUIRE(!expanded.empty());
    // パスの後続部分が残っていること
    REQUIRE(expanded.find("GIMP") != std::string::npos);
}

TEST_CASE("ExpandPlaceholders: %PROGRAMFILES% expands on Windows", "[config]")
{
    std::string path = "%PROGRAMFILES%\\GIMP 3\\lib\\gimp\\3.0\\plug-ins";
    std::string expanded = ExpandPlaceholders(path);

    REQUIRE(expanded.find("%PROGRAMFILES%") == std::string::npos);
    REQUIRE(!expanded.empty());
    REQUIRE(expanded.find("GIMP") != std::string::npos);
}

TEST_CASE("ExpandPlaceholders: path without placeholders is unchanged on Windows", "[config]")
{
    std::string path = "C:/Program Files/GIMP 3/lib/gimp/3.0/plug-ins";
    std::string expanded = ExpandPlaceholders(path);

    REQUIRE(expanded == path);
}

#else // Mac

TEST_CASE("ExpandPlaceholders: {HOME} expands on Mac", "[config]")
{
    std::string path = "{HOME}/Library/Application Support/GIMP/3.0/plug-ins";
    std::string expanded = ExpandPlaceholders(path);

    REQUIRE(expanded.find("{HOME}") == std::string::npos);
    REQUIRE(!expanded.empty());
    REQUIRE(expanded.find("Library") != std::string::npos);
}

TEST_CASE("ExpandPlaceholders: path without placeholders is unchanged on Mac", "[config]")
{
    std::string path =
        "/Applications/GIMP.app/Contents/Resources/lib/gimp/3.0/plug-ins";
    std::string expanded = ExpandPlaceholders(path);

    REQUIRE(expanded == path);
}

#endif

// ---------------------------------------------------------------------------
// LoadConfig テスト
// ---------------------------------------------------------------------------

TEST_CASE("LoadConfig: missing file returns defaults", "[config]")
{
    BridgeConfig cfg = LoadConfig("nonexistent_bridge_config.json");

    REQUIRE(!cfg.pluginSearchPaths.empty());
    // デフォルトパスには GIMP という文字列が含まれる
    REQUIRE(cfg.pluginSearchPaths[0].find("GIMP") != std::string::npos);
}

TEST_CASE("LoadConfig: valid JSON is parsed correctly", "[config]")
{
    TempFile tmp("test_bridge_config.json", VALID_JSON);
    BridgeConfig cfg = LoadConfig(tmp.Str());

    REQUIRE(!cfg.pluginSearchPaths.empty());
    REQUIRE(!cfg.gimpLibDir.empty());
    REQUIRE(!cfg.cspPluginOutputDir.empty());

#ifdef _WIN32
    // Windows セクションが読まれていること
    // gimp_lib_dir は "C:/Program Files/GIMP 3/bin"
    REQUIRE(cfg.gimpLibDir.find("GIMP") != std::string::npos);
    // pluginSearchPaths に %APPDATA% が展開されていること
    bool hasExpanded = false;
    for (const auto& p : cfg.pluginSearchPaths)
    {
        if (p.find("%APPDATA%") == std::string::npos)
        {
            hasExpanded = true;
        }
    }
    REQUIRE(hasExpanded);
#else
    // Mac セクションが読まれていること
    REQUIRE(cfg.gimpLibDir.find("GIMP") != std::string::npos);
    // {HOME} が展開されていること
    bool hasExpanded = false;
    for (const auto& p : cfg.pluginSearchPaths)
    {
        if (p.find("{HOME}") == std::string::npos)
        {
            hasExpanded = true;
        }
    }
    REQUIRE(hasExpanded);
#endif
}

TEST_CASE("LoadConfig: broken JSON returns defaults", "[config]")
{
    TempFile tmp("test_broken_config.json", "{ this is not valid json }");
    BridgeConfig cfg = LoadConfig(tmp.Str());

    REQUIRE(!cfg.pluginSearchPaths.empty());
}

TEST_CASE("LoadConfig: missing platform section returns defaults", "[config]")
{
    // 両プラットフォームセクションとも存在しない JSON
    TempFile tmp("test_noplat_config.json", R"({ "other": {} })");
    BridgeConfig cfg = LoadConfig(tmp.Str());

    REQUIRE(!cfg.pluginSearchPaths.empty());
}

TEST_CASE("LoadConfig: empty plugin_search_paths falls back to defaults", "[config]")
{
    constexpr const char* emptyPathsJson = R"({
        "windows": {
            "plugin_search_paths": [],
            "gimp_lib_dir": "C:/GIMP/bin",
            "csp_plugin_output_dir": "C:/CSP/plugins"
        },
        "mac": {
            "plugin_search_paths": [],
            "gimp_lib_dir": "/Applications/GIMP.app/Contents/Resources/lib",
            "csp_plugin_output_dir": "/Library/CSP/plugins"
        }
    })";

    TempFile tmp("test_emptypaths_config.json", emptyPathsJson);
    BridgeConfig cfg = LoadConfig(tmp.Str());

    // 空配列のときはデフォルトパスにフォールバックする
    REQUIRE(!cfg.pluginSearchPaths.empty());
    // その他フィールドは JSON から取得できている
    REQUIRE(!cfg.gimpLibDir.empty());
    REQUIRE(!cfg.cspPluginOutputDir.empty());
}

// ---------------------------------------------------------------------------
// FindPluginExe テスト
// ---------------------------------------------------------------------------

TEST_CASE("FindPluginExe: returns empty string when not found", "[config]")
{
    BridgeConfig cfg;
    cfg.pluginSearchPaths = { "C:/NonExistentPath/nowhere" };

    std::string result = FindPluginExe(cfg, "plug-in-gauss");
    REQUIRE(result.empty());
}

TEST_CASE("FindPluginExe: returns empty string with empty search paths", "[config]")
{
    BridgeConfig cfg;
    // pluginSearchPaths は空のまま

    std::string result = FindPluginExe(cfg, "plug-in-blur");
    REQUIRE(result.empty());
}

TEST_CASE("FindPluginExe: finds existing file in search path", "[config]")
{
    // 一時ディレクトリにダミー EXE を作成する
    std::filesystem::path tmpDir = std::filesystem::temp_directory_path() / "cspbridge_test_exedir";
    std::filesystem::create_directories(tmpDir);

#ifdef _WIN32
    const std::string exeName = "plug-in-dummy.exe";
#else
    const std::string exeName = "plug-in-dummy";
#endif

    std::filesystem::path exePath = tmpDir / exeName;

    // ダミーファイルを作成
    { std::ofstream ofs(exePath); }

    BridgeConfig cfg;
    cfg.pluginSearchPaths = { tmpDir.string() };

    std::string result = FindPluginExe(cfg, "plug-in-dummy");
    REQUIRE(!result.empty());
    REQUIRE(std::filesystem::exists(result));

    // クリーンアップ
    std::error_code ec;
    std::filesystem::remove(exePath, ec);
    std::filesystem::remove(tmpDir, ec);
}

TEST_CASE("FindPluginExe: searches paths in order and returns first match", "[config]")
{
    // 2 つのディレクトリを用意し、2 番目にのみダミー EXE を置く
    std::filesystem::path tmpDir1 =
        std::filesystem::temp_directory_path() / "cspbridge_test_dir1";
    std::filesystem::path tmpDir2 =
        std::filesystem::temp_directory_path() / "cspbridge_test_dir2";
    std::filesystem::create_directories(tmpDir1);
    std::filesystem::create_directories(tmpDir2);

#ifdef _WIN32
    const std::string exeName = "plug-in-order.exe";
#else
    const std::string exeName = "plug-in-order";
#endif

    std::filesystem::path exePath2 = tmpDir2 / exeName;
    { std::ofstream ofs(exePath2); }

    BridgeConfig cfg;
    cfg.pluginSearchPaths = { tmpDir1.string(), tmpDir2.string() };

    std::string result = FindPluginExe(cfg, "plug-in-order");
    // dir1 にはなく dir2 にある — dir2 のパスが返ること
    REQUIRE(!result.empty());
    REQUIRE(result.find(tmpDir2.string()) != std::string::npos);

    // クリーンアップ
    std::error_code ec;
    std::filesystem::remove(exePath2, ec);
    std::filesystem::remove(tmpDir1, ec);
    std::filesystem::remove(tmpDir2, ec);
}

/**
 * @file   test_plugin_iface.cpp
 * @brief  プラグイン層インターフェースのユニットテスト
 *
 * `CreateAsciiString` ヘルパーと `GetPluginInfo()` の既定値・実装値を検証する。
 * SetupProperty / BuildFilterParams / OnPropertyChanged は CSP API を必要とするため
 * ここでは扱わず、checkerboard E2E（test_checkerboard.exe）で検証する。
 *
 * @author CSPBridgeGimp
 * @date   2026-05-04
 */
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "plugins/plugin_iface.h"

// ---------------------------------------------------------------------------
// CreateAsciiString — TriglavPlugInStringService を最小モックして検証
// ---------------------------------------------------------------------------

namespace
{

// モック: createWithAsciiStringProc が受け取った引数を検証用に外部に保存する
struct StringServiceMockState
{
    std::string  capturedText;
    int          capturedLength = 0;
    bool         createCalled   = false;
    bool         releaseCalled  = false;
    void*        sentinel       = nullptr;
};

StringServiceMockState g_mockState;

TriglavPlugInInt MockCreateWithAsciiString(
    TriglavPlugInStringObject* outObj,
    const TriglavPlugInChar*   text,
    const TriglavPlugInInt     length)
{
    g_mockState.createCalled   = true;
    g_mockState.capturedText   = std::string(reinterpret_cast<const char*>(text), length);
    g_mockState.capturedLength = static_cast<int>(length);
    *outObj = reinterpret_cast<TriglavPlugInStringObject>(&g_mockState.sentinel);
    return 0;
}

TriglavPlugInInt MockReleaseString(TriglavPlugInStringObject /*obj*/)
{
    g_mockState.releaseCalled = true;
    return 0;
}

TriglavPlugInStringService MakeMockStringService()
{
    TriglavPlugInStringService svc{};
    svc.createWithAsciiStringProc = &MockCreateWithAsciiString;
    svc.releaseProc               = &MockReleaseString;
    return svc;
}

} // anonymous namespace

TEST_CASE("CreateAsciiString passes text and length to createWithAsciiStringProc",
          "[plugin_iface]")
{
    g_mockState = {};
    auto svc = MakeMockStringService();

    auto obj = CreateAsciiString(&svc, "Check Size");

    REQUIRE(g_mockState.createCalled);
    REQUIRE(g_mockState.capturedText   == "Check Size");
    REQUIRE(g_mockState.capturedLength == 10);
    REQUIRE(obj != nullptr);
}

TEST_CASE("CreateAsciiString handles empty string", "[plugin_iface]")
{
    g_mockState = {};
    auto svc = MakeMockStringService();

    auto obj = CreateAsciiString(&svc, "");

    REQUIRE(g_mockState.createCalled);
    REQUIRE(g_mockState.capturedText.empty());
    REQUIRE(g_mockState.capturedLength == 0);
    REQUIRE(obj != nullptr);
}

TEST_CASE("CreateAsciiString returns nullptr when service is null", "[plugin_iface]")
{
    auto obj = CreateAsciiString(nullptr, "ignored");
    REQUIRE(obj == nullptr);
}

// ---------------------------------------------------------------------------
// PluginInfo の既定値
// ---------------------------------------------------------------------------

TEST_CASE("PluginInfo has sensible defaults", "[plugin_iface]")
{
    PluginInfo info{};

    REQUIRE(info.exeName.empty());
    REQUIRE(info.procName.empty());
    REQUIRE(info.displayName.empty());
    REQUIRE(info.category   == "GIMP Bridge");
    REQUIRE(info.canPreview == false);

    // targetKinds の既定: RGBA + GrayAlpha
    REQUIRE(info.targetKinds.size() == 2);
    REQUIRE(info.targetKinds[0] == kTriglavPlugInFilterTargetKindRasterLayerRGBAlpha);
    REQUIRE(info.targetKinds[1] == kTriglavPlugInFilterTargetKindRasterLayerGrayAlpha);
}

// ---------------------------------------------------------------------------
// checkerboard.cpp の GetPluginInfo() 実装値を検証
// ---------------------------------------------------------------------------

TEST_CASE("checkerboard plugin reports correct metadata", "[plugin_iface][checkerboard]")
{
    const PluginInfo info = GetPluginInfo();

    REQUIRE(info.exeName     == "checkerboard");
    REQUIRE(info.procName    == "plug-in-checkerboard");
    REQUIRE(info.displayName == "Checkerboard");
    REQUIRE(info.category    == "GIMP Bridge");
    REQUIRE(info.canPreview  == false);
    REQUIRE(info.targetKinds.size() == 2);
}

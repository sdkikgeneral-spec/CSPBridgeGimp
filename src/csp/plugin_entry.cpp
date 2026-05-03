/**
 * @file   plugin_entry.cpp
 * @brief  CSP フィルタープラグイン エントリポイント実装
 *
 * PIHSVMain.cpp（CELSYS 公式サンプル）の構造に忠実に従い実装する。
 * 差異が生じると CSP がフィルターをグレーアウトするため、
 * SDK の呼び出し順序・result 管理・PropertyCallBack の有無を正確に一致させる。
 *
 * @author CSPBridgeGimp
 * @date   2026-04-30
 */

// ---------------------------------------------------------------------------
// GIMP_PLUGIN_ID フォールバック
// ---------------------------------------------------------------------------
#ifndef GIMP_PLUGIN_ID
#define GIMP_PLUGIN_ID ""
#endif

// ---------------------------------------------------------------------------
// プラットフォームヘッダー
// ---------------------------------------------------------------------------
#ifdef _WIN32
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ---------------------------------------------------------------------------
// 標準ライブラリ
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// TriglavPlugIn SDK
// ---------------------------------------------------------------------------
#include "TriglavPlugInSDK.h"

// ---------------------------------------------------------------------------
// プロジェクト内モジュール
// ---------------------------------------------------------------------------
#include "plugin_entry.h"
#include "buffer.h"
#include "../config/config.h"
#include "../host/pdb_stubs.h"
#include "../ipc/wire_io.h"
#include "../plugins/plugin_iface.h"

// ---------------------------------------------------------------------------
// Windows: DllMain で HMODULE を保存
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace
{
HMODULE g_hModule = nullptr;
} // anonymous namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
#endif

// ===========================================================================
// DbgLog — C:\Temp\cspbridge.log にデバッグ情報を追記
// ===========================================================================
namespace
{
void DbgLog(const char* msg)
{
#ifdef _WIN32
    // .cpm と同じディレクトリに cspbridge.log を書く
    char path[MAX_PATH] = {};
    if (g_hModule)
    {
        wchar_t wpath[MAX_PATH] = {};
        ::GetModuleFileNameW(g_hModule, wpath, MAX_PATH);
        // wchar → char 変換（ASCII パス前提）
        ::WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);
        // ファイル名部分を "cspbridge.log" に置き換える
        char* lastSep = std::strrchr(path, '\\');
        if (lastSep) std::strcpy(lastSep + 1, "cspbridge.log");
    }
    FILE* f = path[0] ? std::fopen(path, "a") : nullptr;
#else
    FILE* f = std::fopen("/tmp/cspbridge.log", "a");
#endif
    if (f)
    {
        std::fputs(msg, f);
        std::fclose(f);
    }
}
} // anonymous namespace

// ===========================================================================
// GetModuleDir
// ===========================================================================

std::string GetModuleDir()
{
#ifdef _WIN32
    if (!g_hModule)
    {
        throw std::runtime_error("GetModuleDir: g_hModule is null");
    }
    wchar_t buf[MAX_PATH] = {};
    DWORD len = ::GetModuleFileNameW(g_hModule, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
    {
        throw std::runtime_error("GetModuleDir: GetModuleFileNameW failed");
    }
    std::filesystem::path dllPath(buf);
    return dllPath.parent_path().string();
#else
    ::Dl_info info{};
    if (::dladdr(reinterpret_cast<void*>(&TriglavPluginCall), &info) == 0 || !info.dli_fname)
    {
        throw std::runtime_error("GetModuleDir: dladdr failed");
    }
    std::filesystem::path dylibPath(info.dli_fname);
    return dylibPath.parent_path().string();
#endif
}

// ===========================================================================
// BridgeData — ModuleInitialize で確保し、セレクター間で共有する状態
// ===========================================================================

struct BridgeData
{
    TriglavPlugInPropertyService* pPropertyService = nullptr;
};

// ===========================================================================
// FilterPropertyCallBack — CSP がプロパティ変更を通知するコールバック
//
// サンプル（PIHSVMain.cpp）に倣い実関数を登録する。nullptr を渡すと
// CSP がフィルターを有効化しない可能性がある。
// PoC では値変更を検知するだけで再適用はしないため NoModify を返す。
// ===========================================================================

static void TRIGLAV_PLUGIN_CALLBACK FilterPropertyCallBack(
    TriglavPlugInInt*           result,
    TriglavPlugInPropertyObject /*propertyObject*/,
    const TriglavPlugInInt      /*itemKey*/,
    const TriglavPlugInInt      /*notify*/,
    TriglavPlugInPtr            /*data*/)
{
    *result = kTriglavPlugInPropertyCallBackResultNoModify;
}

// ===========================================================================
// TriglavPluginCall — エントリポイント
//
// サンプルに忠実に:
//   - 先頭で *result = kTriglavPlugInCallResultFailed をセット
//   - 各セレクターの成功パスの末尾でのみ Success をセット
//   - pluginServer が NULL なら即座にリターン
// ===========================================================================

extern "C"
TRIGLAV_PLUGIN_DLL_EXTERN void TRIGLAV_PLUGIN_CALLBACK TriglavPluginCall(
    TriglavPlugInInt*        result,
    TriglavPlugInPtr*        data,
    const TriglavPlugInInt   selector,
    TriglavPlugInServer*     pluginServer,
    TriglavPlugInPtr         /*reserved*/)
{
    *result = kTriglavPlugInCallResultFailed;   // サンプルと同じ初期値

    {
        char dbgBuf[64];
        std::snprintf(dbgBuf, sizeof(dbgBuf),
            "CSPBridge: TriglavPluginCall selector=0x%04X\n",
            static_cast<unsigned>(selector));
        DbgLog(dbgBuf);
    }

    if (pluginServer == nullptr)
    {
        DbgLog("CSPBridge: pluginServer is NULL\n");
        return;
    }

    TriglavPlugInHostObject  hostObject  = pluginServer->hostObject;
    TriglavPlugInRecordSuite& recSuite   = pluginServer->recordSuite;

    // -----------------------------------------------------------------------
    // kTriglavPlugInSelectorModuleInitialize
    // -----------------------------------------------------------------------
    if (selector == kTriglavPlugInSelectorModuleInitialize)
    {
        DbgLog("CSPBridge: ModuleInitialize called\n");

        TriglavPlugInModuleInitializeRecord* moduleRec =
            recSuite.moduleInitializeRecord;
        TriglavPlugInStringService* strSvc =
            pluginServer->serviceSuite.stringService;

        if (moduleRec != nullptr && strSvc != nullptr)
        {
            TriglavPlugInInt hostVersion = 0;
            moduleRec->getHostVersionProc(&hostVersion, hostObject);

            char dbgBuf[128];
            std::snprintf(dbgBuf, sizeof(dbgBuf),
                "CSPBridge: hostVersion=%d needVersion=%d\n",
                static_cast<int>(hostVersion),
                static_cast<int>(kTriglavPlugInNeedHostVersion));
            DbgLog(dbgBuf);

            if (hostVersion >= kTriglavPlugInNeedHostVersion)
            {
                const std::string moduleIdStr =
                    std::string("com.cspbridgegimp.") + GIMP_PLUGIN_ID;
                TriglavPlugInStringObject moduleIdObj = nullptr;
                strSvc->createWithAsciiStringProc(
                    &moduleIdObj,
                    moduleIdStr.c_str(),
                    static_cast<TriglavPlugInInt>(moduleIdStr.size()));

                moduleRec->setModuleIDProc(hostObject, moduleIdObj);
                moduleRec->setModuleKindProc(
                    hostObject, kTriglavPlugInModuleSwitchKindFilter);

                strSvc->releaseProc(moduleIdObj);

                // セレクター間で状態を共有する BridgeData を確保
                BridgeData* bd = new BridgeData;
                *data  = bd;
                *result = kTriglavPlugInCallResultSuccess;
                DbgLog("CSPBridge: ModuleInitialize SUCCESS\n");
            }
            else
            {
                DbgLog("CSPBridge: ModuleInitialize FAILED (hostVersion too low)\n");
            }
        }
        else
        {
            DbgLog("CSPBridge: ModuleInitialize FAILED (moduleRec or strSvc null)\n");
        }
    }

    // -----------------------------------------------------------------------
    // kTriglavPlugInSelectorModuleTerminate
    // -----------------------------------------------------------------------
    else if (selector == kTriglavPlugInSelectorModuleTerminate)
    {
        BridgeData* bd = static_cast<BridgeData*>(*data);
        delete bd;
        *data   = nullptr;
        *result = kTriglavPlugInCallResultSuccess;
    }

    // -----------------------------------------------------------------------
    // kTriglavPlugInSelectorFilterInitialize
    //
    // サンプルに忠実に:
    //   - 各 setXxx の戻り値は個別チェックしない（サンプルも同様）
    //   - if-block の末尾で Success をセット
    //   - PropertyCallBack には実関数を渡す
    // -----------------------------------------------------------------------
    else if (selector == kTriglavPlugInSelectorFilterInitialize)
    {
        DbgLog("CSPBridge: FilterInitialize called\n");

        TriglavPlugInStringService*   strSvc  = pluginServer->serviceSuite.stringService;
        TriglavPlugInPropertyService* propSvc = pluginServer->serviceSuite.propertyService;
        TriglavPlugInFilterInitializeRecord* fiRec = TriglavPlugInGetFilterInitializeRecord(&recSuite);

        if (fiRec != nullptr
            && strSvc  != nullptr
            && propSvc != nullptr)
        {
            BridgeData* bd = static_cast<BridgeData*>(*data);
            if (bd != nullptr)
            {
                bd->pPropertyService = propSvc;
            }

            // カテゴリ名

            TriglavPlugInStringObject catNameObj = nullptr;
            strSvc->createWithAsciiStringProc(
                &catNameObj, "GIMP Bridge",
                static_cast<TriglavPlugInInt>(std::strlen("GIMP Bridge")));
            TriglavPlugInFilterInitializeSetFilterCategoryName(
                &recSuite, hostObject, catNameObj, '\0');
            strSvc->releaseProc(catNameObj);

            // フィルター名（GIMP_PLUGIN_ID）
            TriglavPlugInStringObject filterNameObj = nullptr;
            strSvc->createWithAsciiStringProc(
                &filterNameObj, GIMP_PLUGIN_ID,
                static_cast<TriglavPlugInInt>(std::strlen(GIMP_PLUGIN_ID)));
            TriglavPlugInFilterInitializeSetFilterName(
                &recSuite, hostObject, filterNameObj, '\0');
            strSvc->releaseProc(filterNameObj);

            // プレビュー
            TriglavPlugInFilterInitializeSetCanPreview(
                &recSuite, hostObject, kTriglavPlugInBoolFalse);

            // buffer.cpp がサポートするレイヤー種別のみ（RGBAlpha / GrayAlpha）
            TriglavPlugInInt targets[] = {
                kTriglavPlugInFilterTargetKindRasterLayerRGBAlpha,
                kTriglavPlugInFilterTargetKindRasterLayerGrayAlpha,
            };
            TriglavPlugInFilterInitializeSetTargetKinds(
                &recSuite, hostObject, targets,
                static_cast<TriglavPlugInInt>(std::size(targets)));

            // プロパティ作成（GetProperties() から動的生成）
            TriglavPlugInPropertyObject propObj = nullptr;
            propSvc->createProc(&propObj);

            if (propObj != nullptr)
            {
                const std::vector<PropItemDef> propDefs = GetProperties();
                for (const auto& def : propDefs)
                {
                    TriglavPlugInStringObject labelObj = nullptr;
                    strSvc->createWithAsciiStringProc(
                        &labelObj, def.label.c_str(),
                        static_cast<TriglavPlugInInt>(def.label.size()));
                    propSvc->addItemProc(
                        propObj, def.key,
                        kTriglavPlugInPropertyValueTypeInteger,
                        kTriglavPlugInPropertyValueKindDefault,
                        kTriglavPlugInPropertyInputKindDefault,
                        labelObj, '\0');
                    strSvc->releaseProc(labelObj);

                    propSvc->setIntegerValueProc(propObj,        def.key, def.defaultVal);
                    propSvc->setIntegerDefaultValueProc(propObj, def.key, def.defaultVal);
                    propSvc->setIntegerMinValueProc(propObj,     def.key, def.minVal);
                    propSvc->setIntegerMaxValueProc(propObj,     def.key, def.maxVal);
                }

                TriglavPlugInFilterInitializeSetProperty(
                    &recSuite, hostObject, propObj);

                // 実関数ポインターを登録（nullptr は不可）
                TriglavPlugInFilterInitializeSetPropertyCallBack(
                    &recSuite, hostObject,
                    FilterPropertyCallBack, *data);

                propSvc->releaseProc(propObj);
            }

            *result = kTriglavPlugInCallResultSuccess;   // 末尾でのみ Success
            DbgLog("CSPBridge: FilterInitialize SUCCESS\n");
        }
        else
        {
            DbgLog("CSPBridge: FilterInitialize FAILED (null check)\n");
        }
    }

    // -----------------------------------------------------------------------
    // kTriglavPlugInSelectorFilterTerminate
    // -----------------------------------------------------------------------
    else if (selector == kTriglavPlugInSelectorFilterTerminate)
    {
        *result = kTriglavPlugInCallResultSuccess;
    }

    // -----------------------------------------------------------------------
    // kTriglavPlugInSelectorFilterRun
    // -----------------------------------------------------------------------
    else if (selector == kTriglavPlugInSelectorFilterRun)
    {
        DbgLog("CSPBridge: FilterRun called\n");
        TriglavPlugInOffscreenService* offSvc = pluginServer->serviceSuite.offscreenService;

        if (TriglavPlugInGetFilterRunRecord(&recSuite) != nullptr
            && offSvc != nullptr)
        {
            try
            {
                // CSP にフィルター実行開始を通知
                TriglavPlugInInt processResult = 0;
                TriglavPlugInFilterRunProcess(
                    &recSuite, &processResult,
                    hostObject, kTriglavPlugInFilterRunProcessStateStart);

                if (processResult == kTriglavPlugInFilterRunProcessResultExit)
                {
                    DbgLog("CSPBridge: FilterRun Exit from Start\n");
                    *result = kTriglavPlugInCallResultSuccess;
                    return;
                }

                // offscreen / selectRect 取得
                TriglavPlugInOffscreenObject srcOffscreen = nullptr;
                TriglavPlugInFilterRunGetSourceOffscreen(
                    &recSuite, &srcOffscreen, hostObject);

                TriglavPlugInOffscreenObject dstOffscreen = nullptr;
                TriglavPlugInFilterRunGetDestinationOffscreen(
                    &recSuite, &dstOffscreen, hostObject);

                TriglavPlugInRect selectRect{};
                TriglavPlugInFilterRunGetSelectAreaRect(
                    &recSuite, &selectRect, hostObject);

                // CSP バッファ → RGBA 変換
                CspBridge::CspBuffer cspBuf =
                    CspBridge::ReadFromOffscreen(offSvc, srcOffscreen, selectRect);
                std::vector<uint8_t> rgba = CspBridge::CspToRgba(cspBuf);

                // HostContext に RGBA データをコピー
                HostContext ctx(cspBuf.width, cspBuf.height);
                ctx.SetLogCallback(DbgLog);
                {
                    std::unique_lock lock(ctx.Mutex());
                    std::memcpy(ctx.RgbaData(), rgba.data(), rgba.size());
                }

                // bridge_config.json → プラグイン EXE パスを解決
                const std::string moduleDir  = GetModuleDir();
                const std::string configPath = moduleDir + "/config/bridge_config.json";
                const BridgeConfig cfg       = LoadConfig(configPath);

                const PluginInfo info     = GetPluginInfo();
                const std::string exePath = FindPluginExe(cfg, info.exeName);
                {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                        "CSPBridge: FilterRun exePath='%s'\n", exePath.c_str());
                    DbgLog(buf);
                }

                if (exePath.empty())
                {
                    DbgLog("CSPBridge: FilterRun EXE not found -> Abort\n");
                    TriglavPlugInFilterRunProcess(
                        &recSuite, &processResult,
                        hostObject, kTriglavPlugInFilterRunProcessStateAbort);
                    return;
                }

                // PluginSession を生成してフィルターを実行
                {
                    TriglavPlugInPropertyObject propObj = nullptr;
                    TriglavPlugInFilterRunGetProperty(&recSuite, &propObj, hostObject);

                    BridgeData* bd = static_cast<BridgeData*>(*data);
                    FilterParams params = BuildFilterParams(
                        propObj, bd ? bd->pPropertyService : nullptr);

                    const std::string stderrLog = moduleDir + "/cspbridge_stderr.log";
                    PluginSession session(exePath, cfg.gimpLibDir, PluginMode::Run, &ctx, stderrLog);
                    session.RunFilter(params).get();
                }

                // 処理済み RGBA を読み出す
                {
                    std::shared_lock lock(ctx.Mutex());
                    const uint8_t* ptr = ctx.RgbaData();
                    rgba.assign(ptr,
                        ptr + static_cast<ptrdiff_t>(cspBuf.width) * cspBuf.height * 4);
                }

                // RGBA → CSP バッファ形式に変換して書き戻す
                CspBridge::CspBuffer resultBuf =
                    CspBridge::RgbaToCsp(rgba.data(), cspBuf.width, cspBuf.height, cspBuf);
                CspBridge::WriteToOffscreen(offSvc, dstOffscreen, selectRect, resultBuf);

                // 書き戻し完了を CSP に通知
                TriglavPlugInFilterRunUpdateDestinationOffscreenRect(
                    &recSuite, hostObject, &selectRect);

                // フィルター実行終了を通知
                TriglavPlugInFilterRunProcess(
                    &recSuite, &processResult,
                    hostObject, kTriglavPlugInFilterRunProcessStateEnd);

                *result = kTriglavPlugInCallResultSuccess;
                DbgLog("CSPBridge: FilterRun SUCCESS\n");
            }
            catch (const std::exception& ex)
            {
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                    "CSPBridge: FilterRun EXCEPTION: %s\n", ex.what());
                DbgLog(buf);
            }
            catch (...)
            {
                DbgLog("CSPBridge: FilterRun UNKNOWN EXCEPTION\n");
            }
        }
        else
        {
            DbgLog("CSPBridge: FilterRun runRec or offSvc is null\n");
        }
    }
}

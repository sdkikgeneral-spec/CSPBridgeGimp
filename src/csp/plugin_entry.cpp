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
#include <signal.h>
#include <sys/types.h>
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
    TriglavPlugInPropertyService*  pPropertyService  = nullptr;
    TriglavPlugInPropertyService2* pPropertyService2 = nullptr;
    FilterParams                   cachedParams;      ///< PropertyCallBack で更新されるプロパティ値キャッシュ
#ifdef _WIN32
    HANDLE hPlugin = nullptr;  ///< DuplicateHandle で複製した子プロセスハンドル
#else
    pid_t  pidPlugin = 0;      ///< 子プロセス PID
#endif
};

// ===========================================================================
// KillPluginHandle — BridgeData に保存したハンドル / PID でプロセスを kill する
// ===========================================================================

static void KillPluginHandle(BridgeData* bd)
{
    if (!bd) return;
#ifdef _WIN32
    if (bd->hPlugin)
    {
        DWORD exitCode = 0;
        if (::GetExitCodeProcess(bd->hPlugin, &exitCode)
            && exitCode == STILL_ACTIVE)
        {
            DbgLog("CSPBridge: KillPluginHandle: process still alive, killing\n");
            ::TerminateProcess(bd->hPlugin, 1);
        }
        ::CloseHandle(bd->hPlugin);
        bd->hPlugin = nullptr;
    }
    else
    {
        DbgLog("CSPBridge: KillPluginHandle: no handle (already cleaned up)\n");
    }
#else
    if (bd->pidPlugin > 0)
    {
        if (::kill(bd->pidPlugin, 0) == 0)
        {
            DbgLog("CSPBridge: KillPluginHandle: process still alive, killing\n");
            ::kill(bd->pidPlugin, SIGTERM);
        }
        bd->pidPlugin = 0;
    }
    else
    {
        DbgLog("CSPBridge: KillPluginHandle: no PID (already cleaned up)\n");
    }
#endif
}

// ===========================================================================
// FilterPropertyCallBack — CSP がプロパティ変更を通知するコールバック
//
// 静的サンク。BridgeData から PropertyService v1/v2 を取り出し、
// プラグイン層の OnPropertyChanged() に dispatch する。
// プラグインが返した SDK Result 値をそのまま CSP に返却する。
// ===========================================================================

static void TRIGLAV_PLUGIN_CALLBACK FilterPropertyCallBack(
    TriglavPlugInInt*           result,
    TriglavPlugInPropertyObject propObj,
    const TriglavPlugInInt      itemKey,
    const TriglavPlugInInt      notify,
    TriglavPlugInPtr            data)
{
    BridgeData* bd = static_cast<BridgeData*>(data);

    DbgLog("CSPBridge: FilterPropertyCallBack called\n");

    if (bd && notify == kTriglavPlugInPropertyCallBackNotifyValueChanged)
        bd->cachedParams = BuildFilterParams(propObj, bd->pPropertyService, bd->pPropertyService2);

    *result = OnPropertyChanged(
        propObj, itemKey, notify,
        bd ? bd->pPropertyService  : nullptr,
        bd ? bd->pPropertyService2 : nullptr);
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
        DbgLog("CSPBridge: ModuleTerminate called\n");
        BridgeData* bd = static_cast<BridgeData*>(*data);
        KillPluginHandle(bd);
        delete bd;
        *data   = nullptr;
        *result = kTriglavPlugInCallResultSuccess;
        DbgLog("CSPBridge: ModuleTerminate SUCCESS\n");
    }

    // -----------------------------------------------------------------------
    // kTriglavPlugInSelectorFilterInitialize
    //
    // GetPluginInfo() からメタデータを取得し、SetupProperty() に
    // プロパティ UI 構築を委譲する（CSPBridge アライン版）。
    // -----------------------------------------------------------------------
    else if (selector == kTriglavPlugInSelectorFilterInitialize)
    {
        DbgLog("CSPBridge: FilterInitialize called\n");

        TriglavPlugInStringService*    strSvc   = pluginServer->serviceSuite.stringService;
        TriglavPlugInPropertyService*  propSvc  = pluginServer->serviceSuite.propertyService;
        TriglavPlugInPropertyService2* propSvc2 = pluginServer->serviceSuite.propertyService2;
        TriglavPlugInFilterInitializeRecord* fiRec = TriglavPlugInGetFilterInitializeRecord(&recSuite);

        if (fiRec != nullptr
            && strSvc  != nullptr
            && propSvc != nullptr)
        {
            BridgeData* bd = static_cast<BridgeData*>(*data);
            if (bd != nullptr)
            {
                bd->pPropertyService  = propSvc;
                bd->pPropertyService2 = propSvc2;
            }

            const PluginInfo info = GetPluginInfo();

            // カテゴリ名
            TriglavPlugInStringObject catNameObj = CreateAsciiString(strSvc, info.category);
            TriglavPlugInFilterInitializeSetFilterCategoryName(
                &recSuite, hostObject, catNameObj, '\0');
            strSvc->releaseProc(catNameObj);

            // フィルター名（PluginInfo.displayName）
            TriglavPlugInStringObject filterNameObj = CreateAsciiString(strSvc, info.displayName);
            TriglavPlugInFilterInitializeSetFilterName(
                &recSuite, hostObject, filterNameObj, '\0');
            strSvc->releaseProc(filterNameObj);

            // プレビュー
            TriglavPlugInFilterInitializeSetCanPreview(
                &recSuite, hostObject,
                info.canPreview ? kTriglavPlugInBoolTrue : kTriglavPlugInBoolFalse);

            // 対応カラーモード（PluginInfo.targetKinds）
            std::vector<TriglavPlugInInt> targets(
                info.targetKinds.begin(), info.targetKinds.end());
            if (!targets.empty())
            {
                TriglavPlugInFilterInitializeSetTargetKinds(
                    &recSuite, hostObject, targets.data(),
                    static_cast<TriglavPlugInInt>(targets.size()));
            }

            // プロパティ UI はプラグイン層に委譲
            TriglavPlugInPropertyObject propObj = nullptr;
            propSvc->createProc(&propObj);

            if (propObj != nullptr)
            {
                SetupProperty(propObj, strSvc, propSvc, propSvc2);

                // 初期値でキャッシュを作成（FilterRun 内では propSvc を呼べないため）
                if (bd != nullptr)
                    bd->cachedParams = BuildFilterParams(propObj, propSvc, propSvc2);

                TriglavPlugInFilterInitializeSetProperty(
                    &recSuite, hostObject, propObj);

                TriglavPlugInFilterInitializeSetPropertyCallBack(
                    &recSuite, hostObject,
                    FilterPropertyCallBack, *data);

                propSvc->releaseProc(propObj);
            }

            *result = kTriglavPlugInCallResultSuccess;
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
        DbgLog("CSPBridge: FilterTerminate called\n");
        KillPluginHandle(static_cast<BridgeData*>(*data));
        *result = kTriglavPlugInCallResultSuccess;
        DbgLog("CSPBridge: FilterTerminate SUCCESS\n");
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
            BridgeData* bdRun = static_cast<BridgeData*>(*data);

            try
            {
                // offscreen / selectRect 取得（ループ外：Restart でも変わらない）
                TriglavPlugInOffscreenObject srcOffscreen = nullptr;
                TriglavPlugInFilterRunGetSourceOffscreen(
                    &recSuite, &srcOffscreen, hostObject);

                TriglavPlugInOffscreenObject dstOffscreen = nullptr;
                TriglavPlugInFilterRunGetDestinationOffscreen(
                    &recSuite, &dstOffscreen, hostObject);

                TriglavPlugInRect selectRect{};
                TriglavPlugInFilterRunGetSelectAreaRect(
                    &recSuite, &selectRect, hostObject);

                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "CSPBridge: FilterRun src=%p dst=%p rect=(%d,%d,%d,%d)\n",
                        static_cast<void*>(srcOffscreen),
                        static_cast<void*>(dstOffscreen),
                        selectRect.left, selectRect.top,
                        selectRect.right, selectRect.bottom);
                    DbgLog(buf);
                }

                if (srcOffscreen == nullptr || dstOffscreen == nullptr)
                {
                    DbgLog("CSPBridge: FilterRun offscreen null -> skip\n");
                    *result = kTriglavPlugInCallResultSuccess;
                    return;
                }

                if (selectRect.right <= selectRect.left ||
                    selectRect.bottom <= selectRect.top)
                {
                    DbgLog("CSPBridge: FilterRun selectRect empty -> skip\n");
                    *result = kTriglavPlugInCallResultSuccess;
                    return;
                }

                // bridge_config.json → プラグイン EXE パスを解決（ループ外）
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
                    DbgLog("CSPBridge: FilterRun EXE not found -> skip\n");
                    return;  // *result = Failed のまま返す（設定ミスを明示的にエラーとして通知）
                }

                const std::string stderrLog = moduleDir + "/cspbridge_stderr.log";

                // SDK 仕様に従う while ループ:
                //   StateStart → 処理 → StateEnd
                //   StateEnd が Restart を返したらパラメーターを更新して再実行
                //   StateEnd が Exit を返したら（OK ボタン）ループ終了
                bool restart = true;
                while (true)
                {
                    if (restart)
                    {
                        restart = false;

                        DbgLog("CSPBridge: calling ProcessStateStart\n");
                        TriglavPlugInInt startResult = 0;
                        TriglavPlugInFilterRunProcess(
                            &recSuite, &startResult,
                            hostObject, kTriglavPlugInFilterRunProcessStateStart);
                        {
                            char dbg[64];
                            std::snprintf(dbg, sizeof(dbg),
                                "CSPBridge: ProcessStateStart returned %d\n",
                                static_cast<int>(startResult));
                            DbgLog(dbg);
                        }

                        if (startResult == kTriglavPlugInFilterRunProcessResultExit)
                        {
                            DbgLog("CSPBridge: FilterRun cancelled at Start\n");
                            break;
                        }

                        // PropertyCallBack が更新した最新パラメーターを使用
                        FilterParams params = bdRun ? bdRun->cachedParams : FilterParams{};

                        // ProcessStateStart 後にピクセルデータを読み取る（Start 前は無効）
                        CspBridge::CspBuffer cspBuf =
                            CspBridge::ReadFromOffscreen(offSvc, srcOffscreen, selectRect);

                        // HostContext にソース RGBA をロード
                        std::vector<uint8_t> rgba = CspBridge::CspToRgba(cspBuf);
                        HostContext ctx(cspBuf.width, cspBuf.height);
                        ctx.SetLogCallback(DbgLog);
                        {
                            std::unique_lock lock(ctx.Mutex());
                            std::memcpy(ctx.RgbaData(), rgba.data(), rgba.size());
                        }

                        // GIMP プロセス起動・実行
                        {
                            PluginSession session(exePath, cfg.gimpLibDir, PluginMode::Run, &ctx, stderrLog);

#ifdef _WIN32
                            if (bdRun)
                            {
                                ::DuplicateHandle(
                                    ::GetCurrentProcess(), session.GetProcessHandle(),
                                    ::GetCurrentProcess(), &bdRun->hPlugin,
                                    PROCESS_TERMINATE, FALSE, 0);
                            }
#else
                            if (bdRun) bdRun->pidPlugin = session.GetPid();
#endif

                            session.RunFilter(params).get();

#ifdef _WIN32
                            if (bdRun && bdRun->hPlugin)
                            {
                                ::CloseHandle(bdRun->hPlugin);
                                bdRun->hPlugin = nullptr;
                            }
#else
                            if (bdRun) bdRun->pidPlugin = 0;
#endif
                        }

                        // 処理済み RGBA を読み出して書き戻す
                        {
                            std::shared_lock lock(ctx.Mutex());
                            const uint8_t* ptr = ctx.RgbaData();
                            rgba.assign(ptr,
                                ptr + static_cast<ptrdiff_t>(cspBuf.width) * cspBuf.height * 4);
                        }

                        CspBridge::CspBuffer resultBuf =
                            CspBridge::RgbaToCsp(rgba.data(), cspBuf.width, cspBuf.height, cspBuf);
                        CspBridge::WriteToOffscreen(offSvc, dstOffscreen, selectRect, resultBuf);
                        TriglavPlugInFilterRunUpdateDestinationOffscreenRect(
                            &recSuite, hostObject, &selectRect);
                    }

                    // 処理完了を通知（SDK 仕様: StateEnd → Restart or Exit のみ）
                    TriglavPlugInInt endResult = 0;
                    TriglavPlugInFilterRunProcess(
                        &recSuite, &endResult,
                        hostObject, kTriglavPlugInFilterRunProcessStateEnd);

                    if (endResult == kTriglavPlugInFilterRunProcessResultRestart)
                        restart = true;
                    else
                        break;
                }

                *result = kTriglavPlugInCallResultSuccess;
                DbgLog("CSPBridge: FilterRun SUCCESS\n");
            }
            catch (const std::exception& ex)
            {
                KillPluginHandle(static_cast<BridgeData*>(*data));
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                    "CSPBridge: FilterRun EXCEPTION: %s\n", ex.what());
                DbgLog(buf);
            }
            catch (...)
            {
                KillPluginHandle(static_cast<BridgeData*>(*data));
                DbgLog("CSPBridge: FilterRun UNKNOWN EXCEPTION\n");
            }
        }
        else
        {
            DbgLog("CSPBridge: FilterRun runRec or offSvc is null\n");
        }
    }
}

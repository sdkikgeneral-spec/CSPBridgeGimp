/**
 * @file   plugin_entry.cpp
 * @brief  CSP フィルタープラグイン エントリポイント実装
 *
 * TriglavPluginCall を実装し、セレクターに従って
 * モジュール初期化 / フィルター初期化 / フィルター実行 を処理する。
 *
 * マルチプラグイン DLL アーキテクチャ（spec.md §4）:
 *   ビルド時に -DGIMP_PLUGIN_ID="plug-in-gauss" 等が定義され、
 *   1 つのソースツリーから複数の CSPBridgeGimp_<Id>.dll を生成する。
 *   実行時は config/bridge_config.json で EXE パスを解決する。
 *
 * 実行フロー（HandleFilterRun、spec.md §11）:
 *   1. CSP offscreen から選択矩形内ピクセルを読み込む  (buffer.cpp)
 *   2. RGBA に変換                                    (buffer.cpp)
 *   3. HostContext に書き込む                         (pdb_stubs.cpp)
 *   4. PluginSession を生成して GIMP プラグインを起動  (wire_io.cpp)
 *   5. RunFilter().get() で完了待機
 *   6. HostContext から RGBA を読み出して CSP に書き戻す (buffer.cpp)
 *   7. updateDestinationOffscreenRectProc で書き戻し完了通知
 *
 * @author CSPBridgeGimp
 * @date   2026-04-29
 */

// ---------------------------------------------------------------------------
// GIMP_PLUGIN_ID フォールバック
//
// core_lib（静的ライブラリ）として plugin_entry.cpp をビルドする際には
// GIMP_PLUGIN_ID が定義されていない。
// その場合は空文字列のフォールバックを使用し、実行時に HandleFilterRun 内で
// runtime_error を投げることで安全に失敗する。
// 実際に使用される CSPBridgeGimp_<Id>.dll は meson.build から
// -DGIMP_PLUGIN_ID="<id>" が渡されるため、このフォールバックは使用されない。
// ---------------------------------------------------------------------------
#ifndef GIMP_PLUGIN_ID
#define GIMP_PLUGIN_ID ""
#endif

// ---------------------------------------------------------------------------
// プラットフォームヘッダー
//
// WIN32_LEAN_AND_MEAN / NOMINMAX は meson.build が /D で設定済みのため
// #ifndef ガードで二重定義を防ぐ。
// TriglavPlugInSDK は _WINDOWS を参照するため、_WIN32 ベースで補完する。
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
// src/csp/ からの相対パスで参照する（core_lib には include_directories('src') がない）
// ---------------------------------------------------------------------------
#include "plugin_entry.h"
#include "buffer.h"
#include "../config/config.h"
#include "../host/pdb_stubs.h"
#include "../ipc/wire_io.h"

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
        // スレッドごとの DLL_THREAD_ATTACH / DLL_THREAD_DETACH 通知は不要
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
#else
// Mac: __attribute__((constructor)) は不要。
// dladdr を使うため TriglavPluginCall のアドレスを直接渡す。
#endif

// ===========================================================================
// GetModuleDir
// ===========================================================================

std::string GetModuleDir()
{
#ifdef _WIN32
    if (!g_hModule)
    {
        throw std::runtime_error("GetModuleDir: g_hModule is null (DllMain not called?)");
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
    // TriglavPluginCall のアドレスを使って自分自身の dylib パスを取得
    if (::dladdr(reinterpret_cast<void*>(&TriglavPluginCall), &info) == 0 || !info.dli_fname)
    {
        throw std::runtime_error("GetModuleDir: dladdr failed");
    }
    std::filesystem::path dylibPath(info.dli_fname);
    return dylibPath.parent_path().string();
#endif
}

// ===========================================================================
// 内部ヘルパー — 文字列オブジェクト RAII ラッパー
// ===========================================================================
namespace
{

/**
 * @brief TriglavPlugInStringObject を RAII で管理するヘルパー
 *
 * コンストラクターで createWithAsciiStringProc を呼び、
 * デストラクターで releaseProc を呼ぶ。
 * 使用後の手動 release 忘れを防ぐ。
 */
class ScopedTriglavString
{
public:
    ScopedTriglavString(
        TriglavPlugInStringService* strSvc,
        const char*                 ascii)
        : m_strSvc(strSvc)
        , m_obj(nullptr)
    {
        int len = static_cast<int>(std::strlen(ascii));
        if (strSvc->createWithAsciiStringProc(&m_obj, ascii, len)
            != kTriglavPlugInAPIResultSuccess)
        {
            throw std::runtime_error(
                std::string("ScopedTriglavString: createWithAsciiStringProc failed for: ") + ascii);
        }
    }

    ~ScopedTriglavString()
    {
        if (m_obj && m_strSvc)
        {
            m_strSvc->releaseProc(m_obj);
        }
    }

    TriglavPlugInStringObject Get() const { return m_obj; }

    ScopedTriglavString(const ScopedTriglavString&)            = delete;
    ScopedTriglavString& operator=(const ScopedTriglavString&) = delete;

private:
    TriglavPlugInStringService* m_strSvc;
    TriglavPlugInStringObject   m_obj;
};

// ---------------------------------------------------------------------------
// セレクター処理関数の前方宣言
// ---------------------------------------------------------------------------

void HandleModuleInitialize(
    TriglavPlugInInt*   result,
    TriglavPlugInServer* pluginServer);

void HandleFilterInitialize(
    TriglavPlugInInt*   result,
    TriglavPlugInServer* pluginServer);

void HandleFilterRun(
    TriglavPlugInInt*   result,
    TriglavPlugInServer* pluginServer);

} // anonymous namespace

// ===========================================================================
// TriglavPluginCall — エントリポイント
// ===========================================================================

/**
 * @brief  CSP が呼び出す唯一のエクスポート関数
 *
 * セレクター値に応じて各 Handle* 関数に委譲する。
 * std::exception をキャッチした場合は *result = kTriglavPlugInCallResultFailed を設定する。
 *
 * @param result        呼び出し結果（成功: kTriglavPlugInCallResultSuccess = 0）
 * @param data          プラグイン独自データポインタ（PoC では未使用）
 * @param selector      処理種別（kTriglavPlugInSelectorModuleInitialize 等）
 * @param pluginServer  SDK が提供するサーバーオブジェクト
 * @param reserved      予約（使用禁止）
 */
extern "C"
TRIGLAV_PLUGIN_DLL_EXTERN void TRIGLAV_PLUGIN_CALLBACK TriglavPluginCall(
    TriglavPlugInInt*   result,
    TriglavPlugInPtr*   /*data*/,
    const TriglavPlugInInt selector,
    TriglavPlugInServer* pluginServer,
    TriglavPlugInPtr    /*reserved*/)
{
    *result = kTriglavPlugInCallResultSuccess;

    try
    {
        switch (selector)
        {
        case kTriglavPlugInSelectorModuleInitialize:
            HandleModuleInitialize(result, pluginServer);
            break;

        case kTriglavPlugInSelectorFilterInitialize:
            HandleFilterInitialize(result, pluginServer);
            break;

        case kTriglavPlugInSelectorFilterRun:
            HandleFilterRun(result, pluginServer);
            break;

        case kTriglavPlugInSelectorFilterTerminate:
            // no-op (PoC)
            break;

        case kTriglavPlugInSelectorModuleTerminate:
            // no-op (PoC)
            break;

        default:
            // 未知のセレクターは無視して Success を返す
            break;
        }
    }
    catch (const std::exception&)
    {
        *result = kTriglavPlugInCallResultFailed;
    }
    catch (...)
    {
        *result = kTriglavPlugInCallResultFailed;
    }
}

// ===========================================================================
// 内部ハンドラー実装
// ===========================================================================

namespace
{

// ---------------------------------------------------------------------------
// HandleModuleInitialize
// ---------------------------------------------------------------------------

/**
 * @brief  kTriglavPlugInSelectorModuleInitialize 処理
 *
 * モジュール ID を "com.cspbridgegimp.<GIMP_PLUGIN_ID>" に設定し、
 * モジュール種別を kTriglavPlugInModuleKindFilter に設定する。
 *
 * @param result        呼び出し結果（失敗時に kTriglavPlugInCallResultFailed を設定）
 * @param pluginServer  SDK サーバーオブジェクト
 */
void HandleModuleInitialize(
    TriglavPlugInInt*   result,
    TriglavPlugInServer* pluginServer)
{
    auto* hostObject = pluginServer->hostObject;
    auto& recSuite   = pluginServer->recordSuite;
    auto* strSvc     = pluginServer->serviceSuite.stringService;

    TriglavPlugInModuleInitializeRecord* moduleRec = recSuite.moduleInitializeRecord;
    if (!moduleRec)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }

    // モジュール ID 文字列を構築: "com.cspbridgegimp.<GIMP_PLUGIN_ID>"
    const std::string moduleIdStr = std::string("com.cspbridgegimp.") + GIMP_PLUGIN_ID;
    ScopedTriglavString moduleIdObj(strSvc, moduleIdStr.c_str());

    if (moduleRec->setModuleIDProc(hostObject, moduleIdObj.Get())
        != kTriglavPlugInAPIResultSuccess)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }

    if (moduleRec->setModuleKindProc(hostObject, kTriglavPlugInModuleKindFilter)
        != kTriglavPlugInAPIResultSuccess)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }
}

// ---------------------------------------------------------------------------
// HandleFilterInitialize
// ---------------------------------------------------------------------------

/**
 * @brief  kTriglavPlugInSelectorFilterInitialize 処理
 *
 * フィルターカテゴリ名・フィルター名・プレビュー不可・ターゲット種別を設定する。
 * カテゴリ名: "GIMP Bridge"
 * フィルター名: GIMP_PLUGIN_ID マクロの値
 * TargetKinds: RGBAlpha + GrayAlpha（PoC スコープ）
 *
 * @param result        呼び出し結果（失敗時に kTriglavPlugInCallResultFailed を設定）
 * @param pluginServer  SDK サーバーオブジェクト
 */
void HandleFilterInitialize(
    TriglavPlugInInt*   result,
    TriglavPlugInServer* pluginServer)
{
    auto* hostObject = pluginServer->hostObject;
    auto& recSuite   = pluginServer->recordSuite;
    auto* strSvc     = pluginServer->serviceSuite.stringService;

    TriglavPlugInFilterInitializeRecord* filterInitRec
        = TriglavPlugInGetFilterInitializeRecord(&recSuite);
    if (!filterInitRec)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }

    // カテゴリ名
    ScopedTriglavString categoryName(strSvc, "GIMP Bridge");
    if (TriglavPlugInFilterInitializeSetFilterCategoryName(
            &recSuite, hostObject, categoryName.Get(), '\0')
        != kTriglavPlugInAPIResultSuccess)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }

    // フィルター名（プラグイン ID）
    ScopedTriglavString filterName(strSvc, GIMP_PLUGIN_ID);
    if (TriglavPlugInFilterInitializeSetFilterName(
            &recSuite, hostObject, filterName.Get(), '\0')
        != kTriglavPlugInAPIResultSuccess)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }

    // プレビュー不可
    if (TriglavPlugInFilterInitializeSetCanPreview(
            &recSuite, hostObject, kTriglavPlugInBoolFalse)
        != kTriglavPlugInAPIResultSuccess)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }

    // ターゲット種別（RGBA + GrayAlpha）
    TriglavPlugInInt targetKinds[] = {
        kTriglavPlugInFilterTargetKindRasterLayerRGBAlpha,
        kTriglavPlugInFilterTargetKindRasterLayerGrayAlpha,
    };
    if (TriglavPlugInFilterInitializeSetTargetKinds(
            &recSuite, hostObject, targetKinds,
            static_cast<TriglavPlugInInt>(std::size(targetKinds)))
        != kTriglavPlugInAPIResultSuccess)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }
}

// ---------------------------------------------------------------------------
// HandleFilterRun
// ---------------------------------------------------------------------------

/**
 * @brief  kTriglavPlugInSelectorFilterRun 処理
 *
 * 処理の流れ（spec.md §11）:
 *   1. offscreen / selectRect を CSP から取得
 *   2. ReadFromOffscreen → CspToRgba で RGBA バッファを準備
 *   3. HostContext に RGBA をセット（unique_lock で保護）
 *   4. bridge_config.json をロードしてプラグイン EXE パスを解決
 *   5. PluginSession を生成し RunFilter().get() で完了待機
 *   6. HostContext から RGBA を読み出す（shared_lock で保護）
 *   7. RgbaToCsp → WriteToOffscreen で CSP レイヤーに書き戻し
 *   8. updateDestinationOffscreenRectProc で書き戻し完了を通知
 *
 * @param result        呼び出し結果（失敗時に kTriglavPlugInCallResultFailed を設定）
 * @param pluginServer  SDK サーバーオブジェクト
 * @throws std::runtime_error  CSP API 失敗 / プラグイン EXE 未検出 / PluginSession 失敗
 */
void HandleFilterRun(
    TriglavPlugInInt*   result,
    TriglavPlugInServer* pluginServer)
{
    auto* hostObject = pluginServer->hostObject;
    auto& recSuite   = pluginServer->recordSuite;
    auto* offSvc     = pluginServer->serviceSuite.offscreenService;

    TriglavPlugInFilterRunRecord* runRec = TriglavPlugInGetFilterRunRecord(&recSuite);
    if (!runRec)
    {
        *result = kTriglavPlugInCallResultFailed;
        return;
    }

    // ------------------------------------------------------------------
    // Step 1: CSP から offscreen オブジェクトと選択矩形を取得
    // ------------------------------------------------------------------
    TriglavPlugInOffscreenObject srcOffscreen = nullptr;
    if (runRec->getSourceOffscreenProc(&srcOffscreen, hostObject)
        != kTriglavPlugInAPIResultSuccess)
    {
        throw std::runtime_error("HandleFilterRun: getSourceOffscreenProc failed");
    }

    TriglavPlugInOffscreenObject dstOffscreen = nullptr;
    if (runRec->getDestinationOffscreenProc(&dstOffscreen, hostObject)
        != kTriglavPlugInAPIResultSuccess)
    {
        throw std::runtime_error("HandleFilterRun: getDestinationOffscreenProc failed");
    }

    TriglavPlugInRect selectRect{};
    if (runRec->getSelectAreaRectProc(&selectRect, hostObject)
        != kTriglavPlugInAPIResultSuccess)
    {
        throw std::runtime_error("HandleFilterRun: getSelectAreaRectProc failed");
    }

    // ------------------------------------------------------------------
    // Step 2: CSP バッファ → RGBA 変換
    // ------------------------------------------------------------------
    CspBridge::CspBuffer cspBuf =
        CspBridge::ReadFromOffscreen(offSvc, srcOffscreen, selectRect);

    std::vector<uint8_t> rgba = CspBridge::CspToRgba(cspBuf);

    // ------------------------------------------------------------------
    // Step 3: HostContext に RGBA データをコピー（unique_lock）
    // ------------------------------------------------------------------
    HostContext ctx(cspBuf.width, cspBuf.height);
    {
        std::unique_lock lock(ctx.Mutex());
        std::memcpy(ctx.RgbaData(), rgba.data(), rgba.size());
    }

    // ------------------------------------------------------------------
    // Step 4: bridge_config.json をロード → プラグイン EXE パスを解決
    // ------------------------------------------------------------------
    const std::string moduleDir   = GetModuleDir();
    const std::string configPath  = moduleDir + "/config/bridge_config.json";
    const BridgeConfig cfg        = LoadConfig(configPath);

    const std::string exePath = FindPluginExe(cfg, GIMP_PLUGIN_ID);
    if (exePath.empty())
    {
        throw std::runtime_error(
            std::string("HandleFilterRun: plugin EXE not found for id=") + GIMP_PLUGIN_ID);
    }

    // ------------------------------------------------------------------
    // Step 5: PluginSession を生成してフィルターを実行
    // ------------------------------------------------------------------
    {
        FilterParams params;
        params.procedureName = GIMP_PLUGIN_ID;
        // PoC: image/drawable 以外の追加引数は空（将来 plugins.json から取得）
        params.args          = {};

        PluginSession session(exePath, cfg.gimpLibDir, PluginMode::Run, &ctx);
        session.RunFilter(params).get(); // 完了まで同期待機
    }

    // ------------------------------------------------------------------
    // Step 6: HostContext から処理済み RGBA を読み出す（shared_lock）
    // ------------------------------------------------------------------
    {
        std::shared_lock lock(ctx.Mutex());
        const uint8_t*   ptr = ctx.RgbaData();
        rgba.assign(ptr, ptr + static_cast<ptrdiff_t>(cspBuf.width) * cspBuf.height * 4);
    }

    // ------------------------------------------------------------------
    // Step 7: RGBA → CSP バッファ形式に変換し、オフスクリーンに書き戻す
    // ------------------------------------------------------------------
    CspBridge::CspBuffer resultBuf =
        CspBridge::RgbaToCsp(rgba.data(), cspBuf.width, cspBuf.height, cspBuf);

    CspBridge::WriteToOffscreen(offSvc, dstOffscreen, selectRect, resultBuf);

    // ------------------------------------------------------------------
    // Step 8: 書き戻し完了を CSP に通知
    // ------------------------------------------------------------------
    if (runRec->updateDestinationOffscreenRectProc(hostObject, &selectRect)
        != kTriglavPlugInAPIResultSuccess)
    {
        throw std::runtime_error("HandleFilterRun: updateDestinationOffscreenRectProc failed");
    }
}

} // anonymous namespace

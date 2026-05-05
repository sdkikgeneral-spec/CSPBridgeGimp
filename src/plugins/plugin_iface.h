/**
 * @file   plugin_iface.h
 * @brief  プラグイン層インターフェース定義（CSPBridge アライン版）
 * @author CSPBridgeGimp
 * @date   2026-05-04
 *
 * CSPBridge `Effects/Samples/Blur.cs` / `HSV.cs` の流儀に倣い、各プラグインが
 * SDK を直接呼び出してプロパティ UI を構築する設計。core (`plugin_entry.cpp`)
 * は GIMP wire bridge の共通インフラのみを担い、SDK 型に対する dispatch ロジック
 * を持たない。
 *
 * 各プラグインが実装する 4 関数:
 *   - GetPluginInfo()        : 静的メタデータ
 *   - SetupProperty()        : FilterInitialize 時のプロパティ UI 構築
 *   - BuildFilterParams()    : FilterRun 時の wire 引数組み立て
 *   - OnPropertyChanged()    : プロパティ変更通知のハンドリング
 */

#pragma once

#include <string>
#include <vector>

#include "../ipc/wire_io.h"
#include "TriglavPlugInDefine.h"
#include "TriglavPlugInService.h"

// ---------------------------------------------------------------------------
// メタデータ
// ---------------------------------------------------------------------------

/**
 * @brief プラグイン基本情報
 *
 * `GetPluginInfo()` が返す静的メタデータ。FilterInitialize 時に CSP UI 設定
 * （カテゴリ・名前・プレビュー可否・対応カラーモード）と GIMP プロセス起動
 * （EXE 名・プロシージャ名）の両方の情報源として使われる。
 */
struct PluginInfo
{
    std::string exeName;        ///< GIMP プラグイン EXE 名（拡張子なし、例: "checkerboard"）
    std::string procName;       ///< GIMP プロシージャ名（例: "plug-in-checkerboard"）
    std::string displayName;    ///< CSP UI 上のフィルター表示名（例: "Checkerboard"）
    std::string category = "GIMP Bridge"; ///< CSP UI 上のカテゴリ。既定 "GIMP Bridge"
    bool        canPreview = false;       ///< プレビュー対応。PoC は false 推奨

    /// 対応カラーモード（kTriglavPlugInFilterTargetKindRasterLayer* の値）
    /// 既定: RGBA + GrayAlpha （buffer.cpp が対応する範囲）
    std::vector<int> targetKinds = {
        kTriglavPlugInFilterTargetKindRasterLayerRGBAlpha,
        kTriglavPlugInFilterTargetKindRasterLayerGrayAlpha,
    };
};

// ---------------------------------------------------------------------------
// 各プラグインが実装する 4 関数
// ---------------------------------------------------------------------------

/**
 * @brief  プラグイン基本情報を返す
 * @return PluginInfo の値コピー
 */
PluginInfo GetPluginInfo();

/**
 * @brief  FilterInitialize 時にプロパティ UI を構築する
 *
 * プラグインが SDK の `addItemProc` / `setIntegerValueProc` 等を直接呼んで
 * 自身のプロパティ項目を `propObj` に追加する。CSPBridge `Blur.cs::FilterInitialize`
 * と同じスタイル。
 *
 * Boolean / Integer / Decimal は v1 (`propSvc`) で完結する。
 * Enumeration / String は v2 (`propSvc2`) が必要であり、NULL の場合は
 * その項目をスキップしてログ出力する責務はプラグイン側にある。
 *
 * @param  propObj   propSvc->createProc で確保済みのプロパティオブジェクト
 * @param  strSvc    文字列サービス（ラベル生成用、`CreateAsciiString` のため）
 * @param  propSvc   PropertyService v1（Boolean / Integer / Decimal）
 * @param  propSvc2  PropertyService v2（Enumeration / String、NULL 可）
 */
void SetupProperty(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInStringService*     strSvc,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  propSvc2);

/**
 * @brief  プロパティ値から GIMP wire パラメーターを組み立てる
 *
 * プラグインが `getIntegerValueProc` / `getBooleanValueProc` / `getDecimalValueProc`
 * 等を直接呼んで CSP 側の現在値を読み取り、GIMP プラグインへ送る `FilterParams`
 * を返す。`procedureName` と `args` をすべて埋めること。
 *
 * **呼び出し可能なコンテキスト**: FilterInitialize（`createProc` で生成した propObj）
 * および FilterPropertyCallBack（propObj はコールバック引数として CSP から渡される）。
 * FilterRun 内で `TriglavPlugInFilterRunGetProperty` が返した propObj に対して
 * 呼ぶと CSP SDK 内部で SEH 例外が発生してプロセスがクラッシュする（禁止）。
 *
 * @param  propObj   プラグインが createProc で生成、または callback で受け取った propObj
 * @param  propSvc   PropertyService v1
 * @param  propSvc2  PropertyService v2（NULL 可）
 * @return 子プロセスへ送出する FilterParams
 */
FilterParams BuildFilterParams(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  propSvc2);

/**
 * @brief  プロパティ変更通知のフック
 *
 * CSP からの値変更・ボタン押下・値検証通知を受け取り、SDK の Result 値を返す。
 * 戻り値: `kTriglavPlugInPropertyCallBackResultNoModify` / `Modify` / `Invalid`。
 *
 * 多くの PoC フィルターは `NoModify` 固定で十分。プレビュー対応や inter-property
 * 制約が必要なプラグインのみ意味のある実装をする。
 *
 * @note `PluginInfo::canPreview = true` のプラグインでは、`notify ==
 *       kTriglavPlugInPropertyCallBackNotifyValueChanged` のときに `Modify` を
 *       返してプレビュー再描画をトリガーできる（CSPBridge `Blur.cs::PropertyCallback`
 *       および `HSV.cs::PropertyCallback` 参照）。
 *       FilterRun は SDK 仕様通り while ループで `ProcessStateEnd` を送り、
 *       `Restart` シグナルを受けて再計算する。PropertyCallBack 内での
 *       `propSvc->getXxxValueProc` 呼び出しは公式 SDK ドキュメントで明示されており安全。
 *
 * @param  propObj   現在のプロパティオブジェクト
 * @param  itemKey   通知対象のアイテムキー
 * @param  notify    通知種別（kTriglavPlugInPropertyCallBackNotify*）
 * @param  propSvc   PropertyService v1
 * @param  propSvc2  PropertyService v2（NULL 可）
 * @return SDK の PropertyCallBackResult 値
 */
TriglavPlugInInt OnPropertyChanged(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInInt                itemKey,
    TriglavPlugInInt                notify,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  propSvc2);

// ---------------------------------------------------------------------------
// 共通ヘルパー（plugin_iface.cpp 提供）
// ---------------------------------------------------------------------------

/**
 * @brief  ASCII 文字列から TriglavPlugInStringObject を生成する
 *
 * 呼び出し側は使用後に `strSvc->releaseProc(obj)` を呼ぶ責務がある。
 * CSPBridge `EffectHelper.CreateAsciiString` の C++ 移植。
 *
 * @param  strSvc  文字列サービス（非 NULL）
 * @param  text    ASCII 文字列。非 ASCII 文字を含むと未定義動作
 * @return 生成された TriglavPlugInStringObject。失敗時は nullptr
 */
TriglavPlugInStringObject CreateAsciiString(
    TriglavPlugInStringService* strSvc, const std::string& text);

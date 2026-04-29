/**
 * @file   buffer.h
 * @brief  CSP オフスクリーン ↔ RGBA バッファ変換
 *
 * TriglavPlugIn SDK のオフスクリーン API を使って CSP レイヤーのピクセルデータを
 * flat RGBA 8bpc に変換し、GIMP タイル転送層が期待するフォーマットへ橋渡しする。
 *
 * 設計上の注意点:
 *   - PoC スコープは RGBA 8bpc (kTriglavPlugInOffscreenChannelOrderRGBAlpha) のみ。
 *     それ以外のチャンネルオーダー（CMYK 等）は例外を投げてフェイルファスト。
 *   - セレクション処理は WriteToOffscreen の呼び出し元（plugin_entry.cpp）が行う。
 *     buf の imageData / alphaData には選択領域内ピクセルのみが格納される。
 *   - アルファ取得の優先順位:
 *       1. 分離アルファ (aPixBytes > 0) → alphaData
 *       2. 埋め込みアルファ (pixBytes == 4 かつ alphaPtr == nullptr の場合、
 *          RGB 以外のチャンネルインデックスを effectiveAlphaIdx として使用)
 *       3. なし → 0xFF で埋める
 *
 * @author CSPBridgeGimp
 * @date   2026-04-29
 */
#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "TriglavPlugInService.h"
#include "TriglavPlugInType.h"

namespace CspBridge
{

/**
 * @brief CSP オフスクリーン 1 ブロック分のピクセルデータを保持する構造体
 *
 * ReadFromOffscreen() が生成し、CspToRgba() / WriteToOffscreen() が消費する。
 * imageData / alphaData は width * height * pixBytes / aPixBytes の row-major 配列。
 *
 * effectiveAlphaIdx は ReadFromOffscreen() が計算して格納する。
 * -1 の場合は「分離アルファも埋め込みアルファもない」ことを意味する。
 */
struct CspBuffer
{
    uint32_t             width;          ///< 選択矩形の幅 (pixels)
    uint32_t             height;         ///< 選択矩形の高さ (pixels)
    int32_t              rIdx;           ///< getRGBChannelIndexProc で取得した R チャンネルインデックス
    int32_t              gIdx;           ///< getRGBChannelIndexProc で取得した G チャンネルインデックス
    int32_t              bIdx;           ///< getRGBChannelIndexProc で取得した B チャンネルインデックス
    int32_t              pixBytes;       ///< 画像 1px あたりのバイト数 (通常 3 or 4)
    int32_t              aPixBytes;      ///< 分離アルファ 1px あたりのバイト数 (0 = 分離アルファなし)
    int32_t              effectiveAlphaIdx; ///< 埋め込みアルファのチャンネルインデックス (-1 = なし)
    std::vector<uint8_t> imageData;      ///< width * height * pixBytes、row-major
    std::vector<uint8_t> alphaData;      ///< width * height * aPixBytes (分離アルファありの場合のみ)
};

/**
 * @brief  CSP オフスクリーンから選択矩形内のピクセルを読み込み CspBuffer を返す
 *
 * getBlockRectCountProc → getBlockRectProc → getBlockImageProc / getBlockAlphaProc
 * の呼び出し順で全ブロックを走査し、row-major な imageData / alphaData に詰める。
 *
 * PoC 制約: チャンネルオーダーが kTriglavPlugInOffscreenChannelOrderRGBAlpha 以外の
 * 場合は std::runtime_error を投げる。
 *
 * @param  svc         TriglavPlugInOffscreenService ポインタ (非 null)
 * @param  offscreen   読み取り元オフスクリーンオブジェクト
 * @param  selectRect  選択矩形（CSP キャンバス座標系）
 * @return             読み込んだピクセルデータを保持する CspBuffer
 * @throws std::runtime_error  CSP API 呼び出し失敗、または RGBA 以外のカラーモード
 *
 * @note  この関数は実際の CSP 環境でしか検証できない。
 *        TODO(spec.md §11.1): 実機で getRGBChannelIndexProc の返すインデックス値を
 *        確認し、通常 RGBA レイヤーで rIdx=0, gIdx=1, bIdx=2, effectiveAlphaIdx=3
 *        となることを記録すること。
 */
CspBuffer ReadFromOffscreen(
    const TriglavPlugInOffscreenService* svc,
    TriglavPlugInOffscreenObject         offscreen,
    const TriglavPlugInRect&             selectRect);

/**
 * @brief  CspBuffer の内容を CSP オフスクリーンに書き戻す
 *
 * ReadFromOffscreen() と同じブロック走査順で全ブロックを上書きする。
 * buf は RgbaToCsp() で生成された GIMP 処理済みデータを想定。
 *
 * セレクション処理:
 *   WriteToOffscreen() は選択矩形内のピクセルのみを上書きする。
 *   選択矩形外のピクセルは一切変更されない（バイト単位で不変）。
 *
 * @param  svc         TriglavPlugInOffscreenService ポインタ (非 null)
 * @param  offscreen   書き込み先オフスクリーンオブジェクト
 * @param  selectRect  選択矩形（CSP キャンバス座標系）
 * @param  buf         書き込むピクセルデータ (ReadFromOffscreen() と同じ layout)
 * @throws std::runtime_error  CSP API 呼び出し失敗
 *
 * @note  TODO(spec.md §11.1): updateDestinationOffscreenRectProc の呼び出し要否を
 *        実機で確認すること。現実装では書き戻し後に呼び出さない。
 */
void WriteToOffscreen(
    const TriglavPlugInOffscreenService* svc,
    TriglavPlugInOffscreenObject         offscreen,
    const TriglavPlugInRect&             selectRect,
    const CspBuffer&                     buf);

/**
 * @brief  CspBuffer → flat RGBA 8bpc 変換
 *
 * GIMP の tile_transfer.cpp が期待するフォーマット（R=0, G=1, B=2, A=3）に
 * チャンネルを並べ替えて返す。結果の配列長は width * height * 4。
 *
 * アルファ取得の優先順位（buf.effectiveAlphaIdx を参照）:
 *   1. aPixBytes > 0 → buf.alphaData から 1 バイト取得
 *   2. effectiveAlphaIdx >= 0 → buf.imageData の該当チャンネルを使用
 *   3. それ以外 → 0xFF（完全不透明）
 *
 * @param  buf  変換元 CspBuffer
 * @return      width * height * 4 バイトの flat RGBA 配列
 */
std::vector<uint8_t> CspToRgba(const CspBuffer& buf);

/**
 * @brief  flat RGBA 8bpc → CspBuffer 変換
 *
 * GIMP 処理後の RGBA データを CSP 書き戻し用 CspBuffer に変換する。
 * チャンネル順・pixBytes・aPixBytes は layout から引き継ぎ、
 * imageData / alphaData を新規確保して書き込む。
 *
 * @param  rgba    変換元の flat RGBA 配列 (長さ width * height * 4)
 * @param  width   画像の幅 (pixels)
 * @param  height  画像の高さ (pixels)
 * @param  layout  CspBuffer レイアウト情報（チャンネル順・pixBytes 等を参照）
 * @return         CSP 書き戻し用 CspBuffer
 */
CspBuffer RgbaToCsp(
    const uint8_t*   rgba,
    uint32_t         width,
    uint32_t         height,
    const CspBuffer& layout);

} // namespace CspBridge

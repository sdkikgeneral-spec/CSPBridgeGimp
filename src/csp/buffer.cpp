/**
 * @file   buffer.cpp
 * @brief  CSP オフスクリーン ↔ RGBA バッファ変換の実装
 *
 * TriglavPlugIn SDK の getBlockImageProc / getBlockAlphaProc を使用して
 * CSP レイヤーのピクセルデータを flat RGBA 8bpc に変換する。
 * C# リファレンス実装 (CSPBridgeSolidFill/PixelBuffer.cs) を C++23 に移植。
 *
 * @author CSPBridgeGimp
 * @date   2026-04-29
 */
#include "buffer.h"

#include <cassert>
#include <cstring>
#include <format>
#include <stdexcept>

#include "TriglavPlugInDefine.h"
#include "TriglavPlugInService.h"
#include "TriglavPlugInType.h"

namespace CspBridge
{

// ---------------------------------------------------------------------------
// 内部ヘルパー
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief  getChannelOrderProc を呼び、サポート対象か検証してオーダーを返す
 *
 * サポート対象: RGBAlpha (0x03), GrayAlpha (0x02)。
 * CMYK 等は std::runtime_error を投げる。
 *
 * @param  svc       オフスクリーンサービスポインタ
 * @param  offscreen 対象オフスクリーン
 * @return           チャンネルオーダー定数
 * @throws std::runtime_error  非サポートチャンネルオーダー
 */
TriglavPlugInInt GetAndValidateChannelOrder(
    const TriglavPlugInOffscreenService* svc,
    TriglavPlugInOffscreenObject         offscreen)
{
    TriglavPlugInInt channelOrder = 0;
    if (svc->getChannelOrderProc(&channelOrder, offscreen) != kTriglavPlugInAPIResultSuccess)
    {
        throw std::runtime_error("buffer: getChannelOrderProc failed");
    }

    if (channelOrder == kTriglavPlugInOffscreenChannelOrderRGBAlpha ||
        channelOrder == kTriglavPlugInOffscreenChannelOrderGrayAlpha)
    {
        return channelOrder;
    }

    // CMYK は ICC プロファイルなしのラウンドトリップが色精度上問題があるため PoC 対象外。
    // GIMP Wire Protocol の tile フォーマットは RGBA 8bpc 固定（bpp=4）であり、
    // CMYK tile モードは存在しない。
    // 対処: レイヤーを RGBA またはグレースケールカラーモードに変換してから実行してください。
    throw std::runtime_error(
        std::format("buffer: unsupported channel order 0x{:04x}"
                    " (supported: RGBAlpha=0x{:04x}, GrayAlpha=0x{:04x};"
                    " convert the layer to RGBA or Grayscale color mode first)",
                    static_cast<int>(channelOrder),
                    kTriglavPlugInOffscreenChannelOrderRGBAlpha,
                    kTriglavPlugInOffscreenChannelOrderGrayAlpha));
}

/**
 * @brief  埋め込みアルファのチャンネルインデックスを求める
 *
 * pixBytes == 4 かつ alphaPtr == nullptr の場合、rIdx / gIdx / bIdx 以外の
 * チャンネルインデックスをアルファとして使用する。
 * C# の alphaIdx フォールバックと同じロジック。
 *
 * @param  rIdx      R チャンネルインデックス
 * @param  gIdx      G チャンネルインデックス
 * @param  bIdx      B チャンネルインデックス
 * @param  pixBytes  1 ピクセルあたりのバイト数
 * @param  alphaPtr  分離アルファバッファポインタ (nullptr = 分離アルファなし)
 * @return           埋め込みアルファのチャンネルインデックス。存在しなければ -1
 */
int32_t FindEmbeddedAlphaIdx(
    int32_t  rIdx,
    int32_t  gIdx,
    int32_t  bIdx,
    int32_t  pixBytes,
    const void* alphaPtr) noexcept
{
    if (alphaPtr != nullptr || pixBytes != 4)
    {
        return -1;
    }

    for (int32_t ch = 0; ch < 4; ++ch)
    {
        if (ch != rIdx && ch != gIdx && ch != bIdx)
        {
            return ch;
        }
    }

    return -1;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 公開関数実装
// ---------------------------------------------------------------------------

CspBuffer ReadFromOffscreen(
    const TriglavPlugInOffscreenService* svc,
    TriglavPlugInOffscreenObject         offscreen,
    const TriglavPlugInRect&             selectRect)
{
    assert(svc != nullptr);

    const auto channelOrder = GetAndValidateChannelOrder(svc, offscreen);
    const bool isGray = (channelOrder == kTriglavPlugInOffscreenChannelOrderGrayAlpha);

    const auto width  = static_cast<uint32_t>(selectRect.right  - selectRect.left);
    const auto height = static_cast<uint32_t>(selectRect.bottom - selectRect.top);

    if (width == 0 || height == 0)
    {
        throw std::runtime_error("buffer: ReadFromOffscreen received empty selectRect");
    }

    // グレースケールは RGB チャンネルインデックスの概念がない（rIdx=gIdx=bIdx=0 固定）
    TriglavPlugInInt rIdx = 0, gIdx = 0, bIdx = 0;
    if (!isGray)
    {
        if (svc->getRGBChannelIndexProc(&rIdx, &gIdx, &bIdx, offscreen) != kTriglavPlugInAPIResultSuccess)
        {
            throw std::runtime_error("buffer: getRGBChannelIndexProc failed");
        }
    }

    TriglavPlugInRect selectRectLocal = selectRect;
    TriglavPlugInInt blockCount = 0;
    if (svc->getBlockRectCountProc(&blockCount, offscreen, &selectRectLocal) != kTriglavPlugInAPIResultSuccess)
    {
        throw std::runtime_error("buffer: getBlockRectCountProc failed");
    }

    // 出力バッファ初期化（pixBytes / aPixBytes は最初のブロックで確定するが
    // 全ブロックで同一であることを前提とする）
    CspBuffer result;
    result.width                = width;
    result.height               = height;
    result.rIdx                 = static_cast<int32_t>(rIdx);
    result.gIdx                 = static_cast<int32_t>(gIdx);
    result.bIdx                 = static_cast<int32_t>(bIdx);
    result.pixBytes             = 0;
    result.aPixBytes            = 0;
    result.effectiveAlphaIdx    = -1;
    result.originalChannelOrder = static_cast<int32_t>(channelOrder);

    // ブロックループ
    for (TriglavPlugInInt i = 0; i < blockCount; ++i)
    {
        TriglavPlugInRect blockRect{};
        if (svc->getBlockRectProc(&blockRect, i, offscreen, &selectRectLocal) != kTriglavPlugInAPIResultSuccess)
        {
            throw std::runtime_error(
                std::format("buffer: getBlockRectProc failed at block {}", static_cast<int>(i)));
        }

        TriglavPlugInPoint pos{ blockRect.left, blockRect.top };
        TriglavPlugInRect  tempRect{};

        TriglavPlugInPtr imgPtr   = nullptr;
        TriglavPlugInInt rowBytes = 0, pixBytes = 0;
        if (svc->getBlockImageProc(&imgPtr, &rowBytes, &pixBytes, &tempRect, offscreen, &pos)
            != kTriglavPlugInAPIResultSuccess)
        {
            throw std::runtime_error(
                std::format("buffer: getBlockImageProc failed at block {}", static_cast<int>(i)));
        }

        if (imgPtr == nullptr)
        {
            continue; // C# の imgPtr == null チェックと同等
        }

        TriglavPlugInPtr alphaPtr   = nullptr;
        TriglavPlugInInt aRowBytes  = 0, aPixBytes = 0;
        // getBlockAlphaProc は失敗しても致命的でない（分離アルファなしの場合は alphaPtr が nullptr）
        svc->getBlockAlphaProc(&alphaPtr, &aRowBytes, &aPixBytes, &tempRect, offscreen, &pos);

        // 初回ブロックでサイズ確定・バッファ確保
        if (result.pixBytes == 0)
        {
            result.pixBytes  = static_cast<int32_t>(pixBytes);
            result.aPixBytes = (alphaPtr != nullptr) ? static_cast<int32_t>(aPixBytes) : 0;
            result.effectiveAlphaIdx = FindEmbeddedAlphaIdx(
                result.rIdx, result.gIdx, result.bIdx,
                result.pixBytes, alphaPtr);

            result.imageData.assign(width * height * static_cast<uint32_t>(result.pixBytes), 0u);
            if (result.aPixBytes > 0)
            {
                result.alphaData.assign(width * height * static_cast<uint32_t>(result.aPixBytes), 0u);
            }
        }

        // ピクセルコピー
        const auto* imgRow   = static_cast<const uint8_t*>(imgPtr);
        const auto* alphaRow = static_cast<const uint8_t*>(alphaPtr);

        const auto selLeft = static_cast<int32_t>(selectRectLocal.left);
        const auto selTop  = static_cast<int32_t>(selectRectLocal.top);

        for (TriglavPlugInInt y = blockRect.top; y < blockRect.bottom; ++y)
        {
            const auto* imgPx   = imgRow;
            const auto* alphaPx = alphaRow;

            const auto dstY = static_cast<uint32_t>(y - selTop);

            for (TriglavPlugInInt x = blockRect.left; x < blockRect.right; ++x)
            {
                const auto dstPxOffset =
                    (dstY * width + static_cast<uint32_t>(x - selLeft))
                    * static_cast<uint32_t>(result.pixBytes);

                std::memcpy(&result.imageData[dstPxOffset], imgPx,
                            static_cast<std::size_t>(result.pixBytes));

                if (result.aPixBytes > 0 && alphaPx != nullptr)
                {
                    const auto dstAlphaOffset =
                        (dstY * width + static_cast<uint32_t>(x - selLeft))
                        * static_cast<uint32_t>(result.aPixBytes);
                    std::memcpy(&result.alphaData[dstAlphaOffset], alphaPx,
                                static_cast<std::size_t>(result.aPixBytes));
                    alphaPx += aPixBytes;
                }

                imgPx += pixBytes;
            }

            imgRow += rowBytes;
            if (result.aPixBytes > 0 && alphaPtr != nullptr)
            {
                alphaRow += aRowBytes;
            }
        }
    }

    // ブロックが 1 つも実データなし（pixBytes が未確定）の場合のフォールバック
    // （空レイヤー等の防御）
    if (result.pixBytes == 0)
    {
        result.pixBytes = 3; // RGB デフォルト
        result.imageData.assign(width * height * 3u, 0u);
    }

    return result;
}

void WriteToOffscreen(
    const TriglavPlugInOffscreenService* svc,
    TriglavPlugInOffscreenObject         offscreen,
    const TriglavPlugInRect&             selectRect,
    const CspBuffer&                     buf)
{
    assert(svc != nullptr);

    const auto channelOrder = GetAndValidateChannelOrder(svc, offscreen);
    const bool isGray = (channelOrder == kTriglavPlugInOffscreenChannelOrderGrayAlpha);

    const auto width = static_cast<uint32_t>(selectRect.right - selectRect.left);

    TriglavPlugInRect selectRectLocal = selectRect;
    TriglavPlugInInt blockCount = 0;
    if (svc->getBlockRectCountProc(&blockCount, offscreen, &selectRectLocal) != kTriglavPlugInAPIResultSuccess)
    {
        throw std::runtime_error("buffer: WriteToOffscreen getBlockRectCountProc failed");
    }

    // グレースケールは getRGBChannelIndexProc が不要（ピクセルコピーは pixBytes 単位の memcpy）
    TriglavPlugInInt rIdx = 0, gIdx = 0, bIdx = 0;
    if (!isGray)
    {
        if (svc->getRGBChannelIndexProc(&rIdx, &gIdx, &bIdx, offscreen) != kTriglavPlugInAPIResultSuccess)
        {
            throw std::runtime_error("buffer: WriteToOffscreen getRGBChannelIndexProc failed");
        }
    }

    for (TriglavPlugInInt i = 0; i < blockCount; ++i)
    {
        TriglavPlugInRect blockRect{};
        if (svc->getBlockRectProc(&blockRect, i, offscreen, &selectRectLocal) != kTriglavPlugInAPIResultSuccess)
        {
            throw std::runtime_error(
                std::format("buffer: WriteToOffscreen getBlockRectProc failed at block {}",
                            static_cast<int>(i)));
        }

        TriglavPlugInPoint pos{ blockRect.left, blockRect.top };
        TriglavPlugInRect  tempRect{};

        TriglavPlugInPtr imgPtr   = nullptr;
        TriglavPlugInInt rowBytes = 0, pixBytes = 0;
        if (svc->getBlockImageProc(&imgPtr, &rowBytes, &pixBytes, &tempRect, offscreen, &pos)
            != kTriglavPlugInAPIResultSuccess)
        {
            throw std::runtime_error(
                std::format("buffer: WriteToOffscreen getBlockImageProc failed at block {}",
                            static_cast<int>(i)));
        }

        if (imgPtr == nullptr)
        {
            continue;
        }

        TriglavPlugInPtr alphaPtr   = nullptr;
        TriglavPlugInInt aRowBytes  = 0, aPixBytes = 0;
        svc->getBlockAlphaProc(&alphaPtr, &aRowBytes, &aPixBytes, &tempRect, offscreen, &pos);

        // 埋め込みアルファは imageData に含まれているため、
        // memcpy で一括コピーすれば自動的に書き戻される。
        // buf.effectiveAlphaIdx は alphaData を別途書き戻す判断に使用。
        auto* imgRow   = static_cast<uint8_t*>(imgPtr);
        auto* alphaRow = static_cast<uint8_t*>(alphaPtr);

        const auto selLeft = static_cast<int32_t>(selectRectLocal.left);
        const auto selTop  = static_cast<int32_t>(selectRectLocal.top);

        for (TriglavPlugInInt y = blockRect.top; y < blockRect.bottom; ++y)
        {
            auto* imgPx   = imgRow;
            auto* alphaPx = alphaRow;

            const auto srcY = static_cast<uint32_t>(y - selTop);

            for (TriglavPlugInInt x = blockRect.left; x < blockRect.right; ++x)
            {
                const auto srcPxOffset =
                    (srcY * width + static_cast<uint32_t>(x - selLeft))
                    * static_cast<uint32_t>(buf.pixBytes);

                std::memcpy(imgPx, &buf.imageData[srcPxOffset],
                            static_cast<std::size_t>(buf.pixBytes));

                if (buf.aPixBytes > 0 && alphaPx != nullptr)
                {
                    const auto srcAlphaOffset =
                        (srcY * width + static_cast<uint32_t>(x - selLeft))
                        * static_cast<uint32_t>(buf.aPixBytes);
                    std::memcpy(alphaPx, &buf.alphaData[srcAlphaOffset],
                                static_cast<std::size_t>(buf.aPixBytes));
                    alphaPx += aPixBytes;
                }

                imgPx += pixBytes;
            }

            imgRow += rowBytes;
            if (buf.aPixBytes > 0 && alphaPtr != nullptr)
            {
                alphaRow += aRowBytes;
            }
        }

        // TODO(spec.md §11.1): 書き戻し後に updateDestinationOffscreenRectProc を
        // 呼ぶべきかどうかを実機で確認する。現在は呼ばない。
    }
}

std::vector<uint8_t> CspToRgba(const CspBuffer& buf)
{
    const auto pixCount = buf.width * buf.height;
    std::vector<uint8_t> rgba(pixCount * 4u);

    const auto rIdx = static_cast<uint32_t>(buf.rIdx);
    const auto gIdx = static_cast<uint32_t>(buf.gIdx);
    const auto bIdx = static_cast<uint32_t>(buf.bIdx);
    const auto pb   = static_cast<uint32_t>(buf.pixBytes);
    const auto apb  = static_cast<uint32_t>(buf.aPixBytes);

    for (uint32_t px = 0; px < pixCount; ++px)
    {
        const auto imgBase   = px * pb;
        const auto rgbaBase  = px * 4u;

        rgba[rgbaBase + 0] = buf.imageData[imgBase + rIdx]; // R
        rgba[rgbaBase + 1] = buf.imageData[imgBase + gIdx]; // G
        rgba[rgbaBase + 2] = buf.imageData[imgBase + bIdx]; // B

        uint8_t alpha = 0xFF;
        if (apb > 0)
        {
            // 分離アルファ (先頭バイトのみ使用)
            alpha = buf.alphaData[px * apb];
        }
        else if (buf.effectiveAlphaIdx >= 0)
        {
            // 埋め込みアルファ
            alpha = buf.imageData[imgBase + static_cast<uint32_t>(buf.effectiveAlphaIdx)];
        }
        // else: 不透明固定

        rgba[rgbaBase + 3] = alpha; // A
    }

    return rgba;
}

CspBuffer RgbaToCsp(
    const uint8_t*   rgba,
    uint32_t         width,
    uint32_t         height,
    const CspBuffer& layout)
{
    CspBuffer result;
    result.width             = width;
    result.height            = height;
    result.rIdx              = layout.rIdx;
    result.gIdx              = layout.gIdx;
    result.bIdx              = layout.bIdx;
    result.pixBytes          = layout.pixBytes;
    result.aPixBytes         = layout.aPixBytes;
    result.effectiveAlphaIdx = layout.effectiveAlphaIdx;

    const auto pixCount = width * height;
    const auto pb       = static_cast<uint32_t>(layout.pixBytes);
    const auto apb      = static_cast<uint32_t>(layout.aPixBytes);

    result.imageData.assign(pixCount * pb,  0u);
    if (apb > 0)
    {
        result.alphaData.assign(pixCount * apb, 0u);
    }

    const auto rIdx = static_cast<uint32_t>(layout.rIdx);
    const auto gIdx = static_cast<uint32_t>(layout.gIdx);
    const auto bIdx = static_cast<uint32_t>(layout.bIdx);

    for (uint32_t px = 0; px < pixCount; ++px)
    {
        const auto rgbaBase = px * 4u;
        const auto imgBase  = px * pb;

        if (pb == 1u)
        {
            // グレースケール: BT.709 輝度式で RGB → gray に縮小
            result.imageData[imgBase] = static_cast<uint8_t>(
                0.2126f * rgba[rgbaBase + 0u]
                + 0.7152f * rgba[rgbaBase + 1u]
                + 0.0722f * rgba[rgbaBase + 2u]
                + 0.5f);

            if (apb > 0)
            {
                result.alphaData[px * apb] = rgba[rgbaBase + 3u];
            }
            continue;
        }

        result.imageData[imgBase + rIdx] = rgba[rgbaBase + 0]; // R
        result.imageData[imgBase + gIdx] = rgba[rgbaBase + 1]; // G
        result.imageData[imgBase + bIdx] = rgba[rgbaBase + 2]; // B

        const uint8_t alpha = rgba[rgbaBase + 3];

        if (apb > 0)
        {
            // 分離アルファに書く（先頭バイトのみ）
            result.alphaData[px * apb] = alpha;
        }
        else if (layout.effectiveAlphaIdx >= 0)
        {
            // 埋め込みアルファに書く
            result.imageData[imgBase + static_cast<uint32_t>(layout.effectiveAlphaIdx)] = alpha;
        }
        // else: アルファなしレイアウト → 書かない
    }

    return result;
}

} // namespace CspBridge

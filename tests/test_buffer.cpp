/**
 * @file   test_buffer.cpp
 * @brief  CspBuffer ↔ RGBA 変換のユニットテスト
 *
 * ReadFromOffscreen / WriteToOffscreen は CSP API を必要とするため
 * ここではテストしない。CspToRgba / RgbaToCsp の純粋変換ロジックのみ検証する。
 *
 * テストケース一覧:
 *   1. Buffer roundtrip: RGBA conversion is lossless
 *   2. Channel reorder: BGR layout (rIdx=2,gIdx=1,bIdx=0)
 *   3. Separate alpha roundtrip: aPixBytes=1
 *   4. Embedded alpha roundtrip: pixBytes=4, aPixBytes=0
 *   5. No alpha: pixBytes=3, aPixBytes=0 → alpha must be 0xFF
 *   6. Non-power-of-two size: 100x80 roundtrip
 *
 * @author CSPBridgeGimp
 * @date   2026-04-29
 */
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "csp/buffer.h"

// ---------------------------------------------------------------------------
// テスト用ヘルパー（anonymous namespace）
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief  テスト用 RGBA バッファ生成
 *
 * 各ピクセルを (R=x%256, G=y%256, B=(x+y)%256, A=255) で埋める。
 *
 * @param  width   幅 (pixels)
 * @param  height  高さ (pixels)
 * @return         width * height * 4 バイトの flat RGBA 配列
 */
std::vector<uint8_t> MakeTestRgba(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rgba(width * height * 4u);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const auto base    = (y * width + x) * 4u;
            rgba[base + 0] = static_cast<uint8_t>(x % 256u);
            rgba[base + 1] = static_cast<uint8_t>(y % 256u);
            rgba[base + 2] = static_cast<uint8_t>((x + y) % 256u);
            rgba[base + 3] = 0xFF;
        }
    }
    return rgba;
}

/**
 * @brief  テスト用 CspBuffer レイアウト雛形生成
 *
 * 実データなし（imageData / alphaData は空）。
 * RgbaToCsp() に渡すレイアウト情報のみを保持する。
 *
 * @param  width        幅 (pixels)
 * @param  height       高さ (pixels)
 * @param  rIdx         R チャンネルインデックス
 * @param  gIdx         G チャンネルインデックス
 * @param  bIdx         B チャンネルインデックス
 * @param  pixBytes     1 ピクセルあたりのバイト数 (通常 3 or 4)
 * @param  aPixBytes    分離アルファ 1px あたりのバイト数 (0 = 分離アルファなし)
 * @return              レイアウト情報を持つ CspBuffer（imageData / alphaData は空）
 */
CspBridge::CspBuffer MakeTestLayout(
    uint32_t width,
    uint32_t height,
    int32_t  rIdx,
    int32_t  gIdx,
    int32_t  bIdx,
    int32_t  pixBytes,
    int32_t  aPixBytes)
{
    CspBridge::CspBuffer layout;
    layout.width    = width;
    layout.height   = height;
    layout.rIdx     = rIdx;
    layout.gIdx     = gIdx;
    layout.bIdx     = bIdx;
    layout.pixBytes = pixBytes;
    layout.aPixBytes = aPixBytes;

    // effectiveAlphaIdx: pixBytes==4 かつ aPixBytes==0 の場合に自動計算
    layout.effectiveAlphaIdx = -1;
    if (aPixBytes == 0 && pixBytes == 4)
    {
        for (int32_t ch = 0; ch < 4; ++ch)
        {
            if (ch != rIdx && ch != gIdx && ch != bIdx)
            {
                layout.effectiveAlphaIdx = ch;
                break;
            }
        }
    }

    return layout;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// テストケース
// ---------------------------------------------------------------------------

/**
 * TC-1: RGBA ⇔ CspBuffer の roundtrip が無損失であることを確認する
 *
 * レイアウト: rIdx=0, gIdx=1, bIdx=2, pixBytes=3, aPixBytes=1
 * （分離アルファあり・チャンネル順は標準 RGB）
 */
TEST_CASE("Buffer roundtrip: RGBA conversion is lossless")
{
    constexpr uint32_t W = 64u;
    constexpr uint32_t H = 64u;

    const auto original = MakeTestRgba(W, H);
    const auto layout   = MakeTestLayout(W, H, 0, 1, 2, 3, 1);

    const auto cspBuf   = CspBridge::RgbaToCsp(original.data(), W, H, layout);
    const auto restored = CspBridge::CspToRgba(cspBuf);

    REQUIRE(original == restored);
}

/**
 * TC-2: チャンネル並べ替え — BGR レイアウト (rIdx=2, gIdx=1, bIdx=0)
 *
 * CspBuffer に BGR 順で格納されたデータが CspToRgba によって
 * 正しく RGB に並び替えられることを確認する。
 */
TEST_CASE("Channel reorder: BGR layout CspToRgba produces correct RGB order")
{
    constexpr uint32_t W = 4u;
    constexpr uint32_t H = 4u;

    // BGR レイアウト: pixBytes=3, aPixBytes=0, no embedded alpha → A=0xFF
    const auto layout = MakeTestLayout(W, H, 2, 1, 0, 3, 0);

    const auto original = MakeTestRgba(W, H);
    const auto cspBuf   = CspBridge::RgbaToCsp(original.data(), W, H, layout);
    const auto restored = CspBridge::CspToRgba(cspBuf);

    REQUIRE(original == restored);

    // 追加検証: imageData に BGR 順で格納されていること
    // ピクセル (0,0): R=0, G=0, B=0
    // pixBytes=3 で rIdx=2(B位置に0), gIdx=1(G位置に0), bIdx=0(R位置に0)
    // → imageData[bIdx=0] = original R, imageData[rIdx=2] = original B
    const auto& img = cspBuf.imageData;
    REQUIRE(img[0] == original[2]); // bIdx=0 に B が格納
    REQUIRE(img[1] == original[1]); // gIdx=1 に G が格納
    REQUIRE(img[2] == original[0]); // rIdx=2 に R が格納
}

/**
 * TC-3: 分離アルファ roundtrip — aPixBytes=1
 *
 * アルファが分離バッファに格納されるレイアウトで、
 * alpha 値が RgbaToCsp → CspToRgba を経て正確に保持されることを確認する。
 */
TEST_CASE("Separate alpha roundtrip: aPixBytes=1 preserves alpha")
{
    constexpr uint32_t W = 8u;
    constexpr uint32_t H = 8u;

    // アルファを多様な値に設定
    auto original = MakeTestRgba(W, H);
    for (uint32_t i = 0; i < W * H; ++i)
    {
        original[i * 4u + 3] = static_cast<uint8_t>(i % 256u);
    }

    // 分離アルファレイアウト: pixBytes=3, aPixBytes=1
    const auto layout   = MakeTestLayout(W, H, 0, 1, 2, 3, 1);
    const auto cspBuf   = CspBridge::RgbaToCsp(original.data(), W, H, layout);
    const auto restored = CspBridge::CspToRgba(cspBuf);

    REQUIRE(original == restored);

    // alphaData が適切に埋まっていること
    REQUIRE(cspBuf.alphaData.size() == W * H);
    for (uint32_t i = 0; i < W * H; ++i)
    {
        REQUIRE(cspBuf.alphaData[i] == static_cast<uint8_t>(i % 256u));
    }
}

/**
 * TC-4: 埋め込みアルファ roundtrip — pixBytes=4, aPixBytes=0
 *
 * pixBytes==4 で分離アルファなしの場合、RGB 以外のチャンネル（effectiveAlphaIdx）が
 * アルファとして使われ、roundtrip で値が保持されることを確認する。
 */
TEST_CASE("Embedded alpha roundtrip: pixBytes=4 uses non-RGB channel as alpha")
{
    constexpr uint32_t W = 8u;
    constexpr uint32_t H = 8u;

    // rIdx=0, gIdx=1, bIdx=2 → effectiveAlphaIdx=3
    const auto layout = MakeTestLayout(W, H, 0, 1, 2, 4, 0);
    REQUIRE(layout.effectiveAlphaIdx == 3);

    auto original = MakeTestRgba(W, H);
    // アルファを多様な値に設定
    for (uint32_t i = 0; i < W * H; ++i)
    {
        original[i * 4u + 3] = static_cast<uint8_t>((i * 7u + 13u) % 256u);
    }

    const auto cspBuf   = CspBridge::RgbaToCsp(original.data(), W, H, layout);
    const auto restored = CspBridge::CspToRgba(cspBuf);

    REQUIRE(original == restored);

    // imageData[3] (effectiveAlphaIdx=3) にアルファが書かれていること
    REQUIRE(cspBuf.imageData.size() == W * H * 4u);
    for (uint32_t i = 0; i < W * H; ++i)
    {
        REQUIRE(cspBuf.imageData[i * 4u + 3u] == original[i * 4u + 3u]);
    }
}

/**
 * TC-5: アルファなし — pixBytes=3, aPixBytes=0 → RGBA の A は 0xFF
 *
 * アルファ情報を持たない 3 バイト/ピクセルレイアウトで、
 * CspToRgba が A チャンネルを常に 0xFF で返すことを確認する。
 */
TEST_CASE("No alpha: pixBytes=3 aPixBytes=0 produces opaque alpha in RGBA")
{
    constexpr uint32_t W = 16u;
    constexpr uint32_t H = 16u;

    // pixBytes=3, aPixBytes=0, effectiveAlphaIdx=-1（自動計算で -1 になる）
    const auto layout = MakeTestLayout(W, H, 0, 1, 2, 3, 0);
    REQUIRE(layout.effectiveAlphaIdx == -1);

    auto original = MakeTestRgba(W, H);

    const auto cspBuf = CspBridge::RgbaToCsp(original.data(), W, H, layout);
    const auto rgba   = CspBridge::CspToRgba(cspBuf);

    REQUIRE(rgba.size() == W * H * 4u);
    for (uint32_t i = 0; i < W * H; ++i)
    {
        // RGB は元データと一致
        REQUIRE(rgba[i * 4u + 0u] == original[i * 4u + 0u]);
        REQUIRE(rgba[i * 4u + 1u] == original[i * 4u + 1u]);
        REQUIRE(rgba[i * 4u + 2u] == original[i * 4u + 2u]);
        // A は常に 0xFF
        REQUIRE(rgba[i * 4u + 3u] == 0xFF);
    }
}

/**
 * TC-6: 幅・高さが 64 の倍数でない roundtrip — 100x80
 *
 * GIMP タイルサイズ (64x64) と無関係な任意サイズでも
 * 変換が正しく動作することを確認する。
 */
TEST_CASE("Non-tile-aligned size: 100x80 RGBA roundtrip is lossless")
{
    constexpr uint32_t W = 100u;
    constexpr uint32_t H = 80u;

    const auto original = MakeTestRgba(W, H);
    // 分離アルファあり、標準 RGB チャンネル順
    const auto layout   = MakeTestLayout(W, H, 0, 1, 2, 3, 1);

    const auto cspBuf   = CspBridge::RgbaToCsp(original.data(), W, H, layout);
    const auto restored = CspBridge::CspToRgba(cspBuf);

    REQUIRE(restored.size() == W * H * 4u);
    REQUIRE(original == restored);
}

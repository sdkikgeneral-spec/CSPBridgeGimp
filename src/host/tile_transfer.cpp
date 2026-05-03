/**
 * @file   tile_transfer.cpp
 * @brief  GIMP タイル転送ハンドラーの実装
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * GIMP Wire Protocol のタイル転送シーケンス（GET / PUT）を実装する。
 * spec.md §10、libgimpbase/gimpprotocol.c (`_gp_tile_req_read`,
 * `_gp_tile_ack_read/write`, `_gp_tile_data_read/write`) に準拠。
 *
 * スレッドローカルの scratch バッファ（最大 64*64*4 = 16384 バイト）を使用し、
 * CSP バッファへのアクセスは std::shared_mutex で保護する。
 */

#include "tile_transfer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <shared_mutex>
#include <stdexcept>

#include "pdb_stubs.h"

// ---------------------------------------------------------------------------
// タイル転送カウンター（診断用）
// ---------------------------------------------------------------------------

std::atomic<int32_t> g_tileGetCount{0};
std::atomic<int32_t> g_tilePutCount{0};

// ---------------------------------------------------------------------------
// タイルローカル scratch バッファ（スレッドごとに独立）
//
// GIMP 標準タイル: 64×64 × 4bpp = 16384 バイト。
// thread_local で各スレッドが独自のバッファを持ち、複数スレッドからの
// 同時呼び出しでもデータが混在しない。
// ---------------------------------------------------------------------------

static constexpr uint32_t TILE_SCRATCH_SIZE = GIMP_TILE_WIDTH * GIMP_TILE_HEIGHT * 4u;

static thread_local std::array<uint8_t, TILE_SCRATCH_SIZE> s_scratch;

// ---------------------------------------------------------------------------
// TileIndex
// ---------------------------------------------------------------------------

uint32_t TileIndex(uint32_t x, uint32_t y, uint32_t imageWidth)
{
    const uint32_t tilesPerRow = (imageWidth + GIMP_TILE_WIDTH - 1u) / GIMP_TILE_WIDTH;
    const uint32_t tileX = x / GIMP_TILE_WIDTH;
    const uint32_t tileY = y / GIMP_TILE_HEIGHT;
    return tileY * tilesPerRow + tileX;
}

// ---------------------------------------------------------------------------
// TileRegionWidth / TileRegionHeight
// ---------------------------------------------------------------------------

uint32_t TileRegionWidth(uint32_t x, uint32_t imageWidth)
{
    if (x >= imageWidth)
        throw std::out_of_range("TileRegionWidth: x out of range");
    const uint32_t remaining = imageWidth - x;
    return (remaining < GIMP_TILE_WIDTH) ? remaining : GIMP_TILE_WIDTH;
}

uint32_t TileRegionHeight(uint32_t y, uint32_t imageHeight)
{
    if (y >= imageHeight)
        throw std::out_of_range("TileRegionHeight: y out of range");
    const uint32_t remaining = imageHeight - y;
    return (remaining < GIMP_TILE_HEIGHT) ? remaining : GIMP_TILE_HEIGHT;
}

// ---------------------------------------------------------------------------
// 内部ヘルパー — タイル番号から (tileX, tileY) を逆算する
// ---------------------------------------------------------------------------

static void TileNumToXY(
    uint32_t  tileNum,
    uint32_t  imageWidth,
    uint32_t& outTileX,
    uint32_t& outTileY)
{
    const uint32_t tilesPerRow = (imageWidth + GIMP_TILE_WIDTH - 1u) / GIMP_TILE_WIDTH;
    if (tilesPerRow == 0u)
        throw WireError("TileNumToXY: imageWidth is 0");
    outTileX = tileNum % tilesPerRow;
    outTileY = tileNum / tilesPerRow;
}

// ---------------------------------------------------------------------------
// 内部ヘルパー — CSP バッファからタイルを scratch に読み込む
//
// RGBA バッファのレイアウト: row-major, stride = imageWidth * 4
// タイル領域: (tileX*64, tileY*64) を起点として (tileW, tileH) px
// ---------------------------------------------------------------------------

static void CopyTileFromBuffer(
    const uint8_t* rgbaBuffer,
    uint32_t       imageWidth,
    uint32_t       imageHeight,
    uint32_t       tileNum,
    uint32_t&      outWidth,
    uint32_t&      outHeight)
{
    uint32_t tileX = 0u;
    uint32_t tileY = 0u;
    TileNumToXY(tileNum, imageWidth, tileX, tileY);

    const uint32_t px = tileX * GIMP_TILE_WIDTH;
    const uint32_t py = tileY * GIMP_TILE_HEIGHT;

    outWidth  = TileRegionWidth(px, imageWidth);
    outHeight = TileRegionHeight(py, imageHeight);

    const uint32_t stride = imageWidth * 4u;

    for (uint32_t row = 0u; row < outHeight; ++row)
    {
        const uint8_t* src = rgbaBuffer + (py + row) * stride + px * 4u;
        uint8_t*       dst = s_scratch.data() + row * outWidth * 4u;
        std::memcpy(dst, src, static_cast<size_t>(outWidth) * 4u);
    }
}

// ---------------------------------------------------------------------------
// 内部ヘルパー — scratch からバッファにタイルを書き戻す
// ---------------------------------------------------------------------------

static void CopyTileToBuffer(
    uint8_t*       rgbaBuffer,
    uint32_t       imageWidth,
    uint32_t       imageHeight,
    uint32_t       tileNum,
    uint32_t       tileWidth,
    uint32_t       tileHeight)
{
    uint32_t tileX = 0u;
    uint32_t tileY = 0u;
    TileNumToXY(tileNum, imageWidth, tileX, tileY);

    const uint32_t px = tileX * GIMP_TILE_WIDTH;
    const uint32_t py = tileY * GIMP_TILE_HEIGHT;

    const uint32_t stride = imageWidth * 4u;

    // クリップ: バッファ範囲外には書かない
    const uint32_t clampedW = std::min(tileWidth,  imageWidth  - px);
    const uint32_t clampedH = std::min(tileHeight, imageHeight - py);

    for (uint32_t row = 0u; row < clampedH; ++row)
    {
        uint8_t*       dst = rgbaBuffer + (py + row) * stride + px * 4u;
        const uint8_t* src = s_scratch.data() + row * tileWidth * 4u;
        std::memcpy(dst, src, static_cast<size_t>(clampedW) * 4u);
    }
}

// ---------------------------------------------------------------------------
// HandleTileRequest
//
// GP_TILE_REQ ペイロード (type uint32 は呼び出し元が消費済み):
//   uint32 drawable_id  (0xFFFFFFFF = -1 は PUT のシグナル)
//   uint32 tile_num
//   uint32 shadow
// ---------------------------------------------------------------------------

void HandleTileRequest(WireChannel& channel, HostContext& ctx)
{
    // GP_TILE_REQ ペイロードを読む
    const uint32_t drawableId = channel.ReadUint32();
    const uint32_t tileNum    = channel.ReadUint32();
    const uint32_t shadow     = channel.ReadUint32();

    constexpr uint32_t BPP         = 4u;
    constexpr uint32_t PUT_SIGNAL  = 0xFFFFFFFFu; // wire 上の -1

    if (drawableId != PUT_SIGNAL)
    {
        ++g_tileGetCount;
        // ---------------------------------------------------------------
        // GET パス: バッファからタイルを読んでプラグインに送信する
        //
        //   host → plugin: GP_TILE_DATA { drawable_id, tile_num, shadow,
        //                                 bpp, width, height, use_shm=0,
        //                                 pixel_data[w*h*4] }
        //   plugin → host: GP_TILE_ACK  { ペイロードなし }
        // ---------------------------------------------------------------
        uint32_t tileW = 0u;
        uint32_t tileH = 0u;

        {
            const uint32_t imgW = ctx.Width();
            const uint32_t imgH = ctx.Height();
            // shared_lock で RGBA バッファを読み取る
            std::shared_lock lock(ctx.Mutex());
            CopyTileFromBuffer(ctx.RgbaData(), imgW, imgH, tileNum, tileW, tileH);
        }

        // GP_TILE_DATA を書く
        channel.WriteUint32(static_cast<uint32_t>(GpMessageType::TileData));
        channel.WriteUint32(drawableId);
        channel.WriteUint32(tileNum);
        channel.WriteUint32(shadow);
        channel.WriteUint32(BPP);
        channel.WriteUint32(tileW);
        channel.WriteUint32(tileH);
        channel.WriteUint32(0u); // use_shm = 0
        channel.WriteBytes(s_scratch.data(), tileW * tileH * BPP);

        // GP_TILE_ACK を受信（ペイロードなし）
        const uint32_t ackType = channel.ReadUint32();
        if (static_cast<GpMessageType>(ackType) != GpMessageType::TileAck)
            throw WireError("HandleTileRequest GET: expected GP_TILE_ACK, got "
                + std::to_string(ackType));
    }
    else
    {
        ++g_tilePutCount;
        // ---------------------------------------------------------------
        // PUT パス: プラグインからタイルを受信してバッファに書き戻す
        //
        // GIMP 本体 (`app/plug-in/gimpplugin-message.c::gimp_plug_in_handle_tile_put`,
        // line 199-208) の PUT プロンプト形式:
        //   drawable_id=-1, tile_num=0, shadow=0, **bpp=0, width=0, height=0**,
        //   use_shm=(shm有無), data=NULL
        // 受信側 `_gp_tile_data_read` は use_shm=0 の時 width*height*bpp バイトを
        // 必ず読むため、bpp/w/h を 0 にしないと pixel_data 待ちでブロックする。
        //
        //   host → plugin: GP_TILE_DATA { -1, 0, 0, 0, 0, 0, use_shm=0 } (空プロンプト)
        //   plugin → host: GP_TILE_DATA { drawable_id, tile_num, shadow,
        //                                 bpp, w, h, use_shm=0,
        //                                 pixel_data[w*h*bpp] }
        //   host → plugin: GP_TILE_ACK  { ペイロードなし }
        // ---------------------------------------------------------------
        (void)tileNum;  // PUT REQ の tile_num は常に 0（無視）
        (void)shadow;   // PUT REQ の shadow は常に 0（無視）

        // GP_TILE_DATA プロンプトを送信（GIMP app 仕様: 全ゼロ）
        channel.WriteUint32(static_cast<uint32_t>(GpMessageType::TileData));
        channel.WriteUint32(PUT_SIGNAL);
        channel.WriteUint32(0u); // tile_num
        channel.WriteUint32(0u); // shadow
        channel.WriteUint32(0u); // bpp = 0
        channel.WriteUint32(0u); // width = 0
        channel.WriteUint32(0u); // height = 0
        channel.WriteUint32(0u); // use_shm = 0
        // pixel_data なし（length = 0*0*0 = 0 なので _gp_tile_data_read は何も読まない）

        // プラグインからの GP_TILE_DATA（実際のピクセルデータ）を受信
        const uint32_t dataType = channel.ReadUint32();
        if (static_cast<GpMessageType>(dataType) != GpMessageType::TileData)
            throw WireError("HandleTileRequest PUT: expected GP_TILE_DATA from plugin, got "
                + std::to_string(dataType));

        const uint32_t recvDrawableId = channel.ReadUint32();
        const uint32_t recvTileNum    = channel.ReadUint32();
        const uint32_t recvShadow     = channel.ReadUint32();
        const uint32_t recvBpp        = channel.ReadUint32();
        const uint32_t recvW          = channel.ReadUint32();
        const uint32_t recvH          = channel.ReadUint32();
        const uint32_t recvUseShm     = channel.ReadUint32();

        (void)recvDrawableId;
        (void)recvShadow;
        (void)recvUseShm;

        const uint32_t pixelBytes = recvW * recvH * recvBpp;
        if (pixelBytes > TILE_SCRATCH_SIZE)
            throw WireError("HandleTileRequest PUT: received tile too large: "
                + std::to_string(pixelBytes));

        channel.ReadBytes(s_scratch.data(), pixelBytes);

        // RGBA バッファに書き戻す（unique_lock）— 実タイル番号は recvTileNum 側
        {
            const uint32_t imgW = ctx.Width();
            const uint32_t imgH = ctx.Height();
            std::unique_lock lock(ctx.Mutex());
            CopyTileToBuffer(ctx.RgbaData(), imgW, imgH, recvTileNum, recvW, recvH);
        }

        // GP_TILE_ACK を送信（ペイロードなし）
        channel.WriteUint32(static_cast<uint32_t>(GpMessageType::TileAck));
    }
}

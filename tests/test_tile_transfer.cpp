/**
 * @file   test_tile_transfer.cpp
 * @brief  src/host/tile_transfer の単体テスト
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * TileIndex / TileRegionWidth / TileRegionHeight の純粋関数テスト、および
 * HandleTileRequest の GET / PUT ラウンドトリップをパイプモックで検証する。
 * 実際の GIMP プロセスは不要（同一プロセス内でホスト側とプラグイン側を模倣）。
 *
 * spec.md §10 / spec_test.md §3 参照。
 */

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "../src/host/pdb_stubs.h"
#include "../src/host/tile_transfer.h"
#include "../src/ipc/wire_io.h"

// ---------------------------------------------------------------------------
// テストヘルパー
// ---------------------------------------------------------------------------

namespace
{

/// pipe() / _pipe() で作成した fd ペアを RAII で管理する
struct PipePair
{
    int readFd  = -1;
    int writeFd = -1;

    PipePair()
    {
#ifdef _WIN32
        int fds[2];
        if (::_pipe(fds, 65536, _O_BINARY) != 0)
            throw std::runtime_error("_pipe failed");
#else
        int fds[2];
        if (::pipe(fds) != 0)
            throw std::runtime_error("pipe failed");
#endif
        readFd  = fds[0];
        writeFd = fds[1];
    }

    ~PipePair()
    {
        closefd(readFd);
        closefd(writeFd);
    }

    static void closefd(int& fd)
    {
        if (fd >= 0)
        {
#ifdef _WIN32
            ::_close(fd);
#else
            ::close(fd);
#endif
            fd = -1;
        }
    }

    PipePair(const PipePair&)            = delete;
    PipePair& operator=(const PipePair&) = delete;
};

/**
 * @brief HandleTileRequest テスト用ダブルパイプフィクスチャ
 *
 * 実際の通信では:
 *   plugin → host (hostReadPipe)
 *   host → plugin (hostWritePipe)
 *
 * テストでは:
 *   hostChannel  : hostReadPipe.readFd,  hostWritePipe.writeFd
 *                  (HandleTileRequest に渡す)
 *   pluginChannel: hostWritePipe.readFd, hostReadPipe.writeFd
 *                  (テストがプラグインを模倣して GP_TILE_REQ 等を書く)
 */
struct TileFixture
{
    PipePair    hostReadPipe;   ///< plugin → host 方向
    PipePair    hostWritePipe;  ///< host → plugin 方向
    WireChannel hostChannel;    ///< HandleTileRequest に渡すチャネル
    WireChannel pluginChannel;  ///< テストがプラグイン側として使うチャネル
    HostContext ctx;

    explicit TileFixture(uint32_t w = 128u, uint32_t h = 128u)
        : hostChannel(hostReadPipe.readFd,   hostWritePipe.writeFd)
        , pluginChannel(hostWritePipe.readFd, hostReadPipe.writeFd)
        , ctx(w, h)
    {
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// TileIndex: 正しい計算
// ---------------------------------------------------------------------------

TEST_CASE("TileIndex: correct calculation", "[tile_transfer]")
{
    // 128px 幅 → tilesPerRow = 2
    REQUIRE(TileIndex(0u,  0u,  128u) == 0u);
    REQUIRE(TileIndex(64u, 0u,  128u) == 1u);
    REQUIRE(TileIndex(0u,  64u, 128u) == 2u);
    REQUIRE(TileIndex(64u, 64u, 128u) == 3u);
}

TEST_CASE("TileIndex: single-column image (width <= 64)", "[tile_transfer]")
{
    // 64px 幅 → tilesPerRow = 1
    REQUIRE(TileIndex(0u, 0u,   64u) == 0u);
    REQUIRE(TileIndex(0u, 64u,  64u) == 1u);
    REQUIRE(TileIndex(0u, 128u, 64u) == 2u);
}

TEST_CASE("TileIndex: non-power-of-two width", "[tile_transfer]")
{
    // 100px 幅 → tilesPerRow = 2 (0..63 と 64..99 の 2 列)
    REQUIRE(TileIndex(0u,  0u, 100u) == 0u);
    REQUIRE(TileIndex(64u, 0u, 100u) == 1u);
    REQUIRE(TileIndex(0u, 64u, 100u) == 2u);
    REQUIRE(TileIndex(64u,64u, 100u) == 3u);
}

// ---------------------------------------------------------------------------
// TileRegionWidth / TileRegionHeight: 端タイルのクリッピング
// ---------------------------------------------------------------------------

TEST_CASE("TileRegionWidth: full tile when not at edge", "[tile_transfer]")
{
    REQUIRE(TileRegionWidth(0u,  128u) == 64u);
    REQUIRE(TileRegionWidth(64u, 128u) == 64u);
}

TEST_CASE("TileRegionWidth: clipped at right edge", "[tile_transfer]")
{
    // 幅 100px の画像: x=64 のタイルは 100 - 64 = 36px
    REQUIRE(TileRegionWidth(64u, 100u) == 36u);
    // 幅 65px の画像: x=64 のタイルは 1px
    REQUIRE(TileRegionWidth(64u, 65u) == 1u);
}

TEST_CASE("TileRegionHeight: full tile when not at edge", "[tile_transfer]")
{
    REQUIRE(TileRegionHeight(0u,  128u) == 64u);
    REQUIRE(TileRegionHeight(64u, 128u) == 64u);
}

TEST_CASE("TileRegionHeight: clipped at bottom edge", "[tile_transfer]")
{
    // 高さ 80px の画像: y=64 のタイルは 80 - 64 = 16px
    REQUIRE(TileRegionHeight(64u, 80u) == 16u);
}

TEST_CASE("TileRegionWidth: throws on out-of-range x", "[tile_transfer]")
{
    REQUIRE_THROWS_AS(TileRegionWidth(128u, 128u), std::out_of_range);
}

TEST_CASE("TileRegionHeight: throws on out-of-range y", "[tile_transfer]")
{
    REQUIRE_THROWS_AS(TileRegionHeight(128u, 128u), std::out_of_range);
}

// ---------------------------------------------------------------------------
// GET ラウンドトリップ
//
// テスト手順:
//   1. HostContext の RGBA バッファに既知のデータを書き込む
//   2. プラグイン側 (pluginChannel) から GP_TILE_REQ を送る
//   3. HandleTileRequest を呼ぶ（ホスト側処理）
//   4. プラグイン側が GP_TILE_DATA を受信し、ピクセルデータを検証
//   5. プラグイン側が GP_TILE_ACK を送る
// ---------------------------------------------------------------------------

TEST_CASE("HandleTileRequest GET: sends correct tile data", "[tile_transfer]")
{
    // 64x64 画像（タイル 1 枚ちょうど）
    TileFixture f(64u, 64u);

    // バッファに既知パターンを書き込む: 各ピクセル (R=x, G=y, B=0xAB, A=0xFF)
    {
        uint8_t* buf = f.ctx.RgbaData();
        for (uint32_t row = 0u; row < 64u; ++row)
        {
            for (uint32_t col = 0u; col < 64u; ++col)
            {
                const uint32_t idx = (row * 64u + col) * 4u;
                buf[idx + 0u] = static_cast<uint8_t>(col);   // R
                buf[idx + 1u] = static_cast<uint8_t>(row);   // G
                buf[idx + 2u] = 0xABu;                        // B
                buf[idx + 3u] = 0xFFu;                        // A
            }
        }
    }

    // プラグイン側: GP_TILE_REQ を送信（drawable_id=1, tile_num=0, shadow=0）
    f.pluginChannel.WriteUint32(1u);  // drawable_id (= DRAWABLE_ID, not -1)
    f.pluginChannel.WriteUint32(0u);  // tile_num
    f.pluginChannel.WriteUint32(0u);  // shadow

    // HandleTileRequest を別スレッドで実行（同一スレッドでは双方向パイプがデッドロック）
    std::thread hostThread([&]()
    {
        HandleTileRequest(f.hostChannel, f.ctx);
    });

    // プラグイン側: GP_TILE_DATA を受信して検証
    const uint32_t msgType = f.pluginChannel.ReadUint32();
    REQUIRE(msgType == static_cast<uint32_t>(GpMessageType::TileData));

    const uint32_t recvDrawableId = f.pluginChannel.ReadUint32();
    const uint32_t recvTileNum    = f.pluginChannel.ReadUint32();
    const uint32_t recvShadow     = f.pluginChannel.ReadUint32();
    const uint32_t recvBpp        = f.pluginChannel.ReadUint32();
    const uint32_t recvW          = f.pluginChannel.ReadUint32();
    const uint32_t recvH          = f.pluginChannel.ReadUint32();
    const uint32_t recvUseShm     = f.pluginChannel.ReadUint32();

    REQUIRE(recvDrawableId == 1u);
    REQUIRE(recvTileNum    == 0u);
    REQUIRE(recvShadow     == 0u);
    REQUIRE(recvBpp        == 4u);
    REQUIRE(recvW          == 64u);
    REQUIRE(recvH          == 64u);
    REQUIRE(recvUseShm     == 0u);

    // ピクセルデータを受信して検証
    const uint32_t pixelBytes = recvW * recvH * recvBpp;
    std::vector<uint8_t> received(pixelBytes);
    f.pluginChannel.ReadBytes(received.data(), pixelBytes);

    // 既知パターンと一致するか確認（最初のいくつかのピクセルをサンプリング）
    REQUIRE(received[0u] == 0u);    // (0,0) R=0
    REQUIRE(received[1u] == 0u);    // (0,0) G=0
    REQUIRE(received[2u] == 0xABu); // (0,0) B
    REQUIRE(received[3u] == 0xFFu); // (0,0) A
    // (1,0): col=1, row=0
    REQUIRE(received[4u] == 1u);    // R=1
    REQUIRE(received[5u] == 0u);    // G=0
    // (0,1): col=0, row=1
    REQUIRE(received[256u] == 0u);  // R=0
    REQUIRE(received[257u] == 1u);  // G=1

    // GP_TILE_ACK を送信（ペイロードなし）
    f.pluginChannel.WriteUint32(static_cast<uint32_t>(GpMessageType::TileAck));

    hostThread.join();
}

TEST_CASE("HandleTileRequest GET: edge tile is clipped correctly", "[tile_transfer]")
{
    // 幅 100px, 高さ 100px の画像: タイル(1,0) = x=64, y=0 → 幅36, 高さ64
    TileFixture f(100u, 100u);

    // バッファを既知値で埋める（赤一色）
    {
        uint8_t* buf = f.ctx.RgbaData();
        for (size_t i = 0u; i < 100u * 100u * 4u; i += 4u)
        {
            buf[i + 0u] = 0xFFu; // R
            buf[i + 1u] = 0x00u; // G
            buf[i + 2u] = 0x00u; // B
            buf[i + 3u] = 0xFFu; // A
        }
    }

    // tile_num=1 → tileX=1, tileY=0 → px=64, py=0 → w=36, h=64
    f.pluginChannel.WriteUint32(1u);  // drawable_id
    f.pluginChannel.WriteUint32(1u);  // tile_num = 1
    f.pluginChannel.WriteUint32(0u);  // shadow

    std::thread hostThread([&]()
    {
        HandleTileRequest(f.hostChannel, f.ctx);
    });

    const uint32_t msgType = f.pluginChannel.ReadUint32();
    REQUIRE(msgType == static_cast<uint32_t>(GpMessageType::TileData));

    f.pluginChannel.ReadUint32(); // drawable_id
    f.pluginChannel.ReadUint32(); // tile_num
    f.pluginChannel.ReadUint32(); // shadow
    f.pluginChannel.ReadUint32(); // bpp
    const uint32_t w  = f.pluginChannel.ReadUint32();
    const uint32_t h  = f.pluginChannel.ReadUint32();
    f.pluginChannel.ReadUint32(); // use_shm

    REQUIRE(w == 36u);
    REQUIRE(h == 64u);

    // ピクセルデータを読み捨てる
    const uint32_t pixelBytes = w * h * 4u;
    std::vector<uint8_t> discard(pixelBytes);
    f.pluginChannel.ReadBytes(discard.data(), pixelBytes);

    // 赤チャンネル確認
    REQUIRE(discard[0u] == 0xFFu);
    REQUIRE(discard[1u] == 0x00u);

    f.pluginChannel.WriteUint32(static_cast<uint32_t>(GpMessageType::TileAck));
    hostThread.join();
}

// ---------------------------------------------------------------------------
// PUT ラウンドトリップ
//
// テスト手順:
//   1. プラグイン側から GP_TILE_REQ (drawable_id=0xFFFFFFFF) を送る
//   2. HandleTileRequest を呼ぶ（ホスト側処理）
//   3. ホストが GP_TILE_DATA プロンプトを送る → プラグイン側が受信
//   4. プラグイン側が GP_TILE_DATA（既知ピクセルデータ）を送る
//   5. ホストが GP_TILE_ACK を送る → プラグイン側が受信
//   6. HostContext のバッファが更新されていることを検証
// ---------------------------------------------------------------------------

TEST_CASE("HandleTileRequest PUT: writes pixel data to HostContext buffer", "[tile_transfer]")
{
    // 64x64 画像（タイル 1 枚）
    TileFixture f(64u, 64u);

    // プラグイン側: GP_TILE_REQ (PUT シグナル: drawable_id = 0xFFFFFFFF)
    f.pluginChannel.WriteUint32(0xFFFFFFFFu); // drawable_id = -1 (PUT)
    f.pluginChannel.WriteUint32(0u);           // tile_num
    f.pluginChannel.WriteUint32(0u);           // shadow

    // HandleTileRequest を別スレッドで実行
    std::thread hostThread([&]()
    {
        HandleTileRequest(f.hostChannel, f.ctx);
    });

    // プラグイン側: GP_TILE_DATA プロンプトを受信
    const uint32_t promptType = f.pluginChannel.ReadUint32();
    REQUIRE(promptType == static_cast<uint32_t>(GpMessageType::TileData));

    const uint32_t promptDrawableId = f.pluginChannel.ReadUint32();
    const uint32_t promptTileNum    = f.pluginChannel.ReadUint32();
    const uint32_t promptShadow     = f.pluginChannel.ReadUint32();
    const uint32_t promptBpp        = f.pluginChannel.ReadUint32();
    const uint32_t promptW          = f.pluginChannel.ReadUint32();
    const uint32_t promptH          = f.pluginChannel.ReadUint32();
    const uint32_t promptUseShm     = f.pluginChannel.ReadUint32();
    // プロンプトに pixel_data はない

    REQUIRE(promptDrawableId == 0xFFFFFFFFu);
    REQUIRE(promptTileNum    == 0u);
    REQUIRE(promptShadow     == 0u);
    REQUIRE(promptBpp        == 4u);
    REQUIRE(promptW          == 64u);
    REQUIRE(promptH          == 64u);
    REQUIRE(promptUseShm     == 0u);

    // プラグイン側: GP_TILE_DATA（既知ピクセルデータ）を送信
    // 既知パターン: 全ピクセルを青 (R=0, G=0, B=0xFF, A=0xFF)
    const uint32_t pixelBytes = 64u * 64u * 4u;
    std::vector<uint8_t> bluePixels(pixelBytes);
    for (size_t i = 0u; i < pixelBytes; i += 4u)
    {
        bluePixels[i + 0u] = 0x00u; // R
        bluePixels[i + 1u] = 0x00u; // G
        bluePixels[i + 2u] = 0xFFu; // B
        bluePixels[i + 3u] = 0xFFu; // A
    }

    f.pluginChannel.WriteUint32(static_cast<uint32_t>(GpMessageType::TileData));
    f.pluginChannel.WriteUint32(1u);    // drawable_id (= DRAWABLE_ID)
    f.pluginChannel.WriteUint32(0u);    // tile_num
    f.pluginChannel.WriteUint32(0u);    // shadow
    f.pluginChannel.WriteUint32(4u);    // bpp
    f.pluginChannel.WriteUint32(64u);   // width
    f.pluginChannel.WriteUint32(64u);   // height
    f.pluginChannel.WriteUint32(0u);    // use_shm
    f.pluginChannel.WriteBytes(bluePixels.data(), pixelBytes);

    // プラグイン側: GP_TILE_ACK を受信
    const uint32_t ackType = f.pluginChannel.ReadUint32();
    REQUIRE(ackType == static_cast<uint32_t>(GpMessageType::TileAck));

    hostThread.join();

    // HostContext バッファが青色で更新されているか検証
    const uint8_t* buf = f.ctx.RgbaData();
    // (0,0)
    REQUIRE(buf[0u] == 0x00u); // R
    REQUIRE(buf[1u] == 0x00u); // G
    REQUIRE(buf[2u] == 0xFFu); // B
    REQUIRE(buf[3u] == 0xFFu); // A
    // (63,63) — 最後のピクセル
    const size_t lastIdx = (63u * 64u + 63u) * 4u;
    REQUIRE(buf[lastIdx + 0u] == 0x00u);
    REQUIRE(buf[lastIdx + 2u] == 0xFFu);
}

// ---------------------------------------------------------------------------
// スレッドローカルバッファの独立性
//
// 2 スレッドが同時に異なるタイルを GET しても、それぞれが独自の scratch
// バッファを使うため干渉しないことを確認する。
// ---------------------------------------------------------------------------

TEST_CASE("HandleTileRequest: thread-local scratch buffers are independent", "[tile_transfer]")
{
    // 128x64 画像: tile[0]=(0,0)→赤, tile[1]=(64,0)→緑
    const uint32_t IMG_W = 128u;
    const uint32_t IMG_H = 64u;
    HostContext ctx(IMG_W, IMG_H);

    // tile[0] を赤、tile[1] を緑で塗る
    uint8_t* buf = ctx.RgbaData();
    for (uint32_t row = 0u; row < IMG_H; ++row)
    {
        for (uint32_t col = 0u; col < IMG_W; ++col)
        {
            const uint32_t idx = (row * IMG_W + col) * 4u;
            if (col < 64u)
            {
                buf[idx + 0u] = 0xFFu; // R (赤)
                buf[idx + 1u] = 0x00u;
                buf[idx + 2u] = 0x00u;
                buf[idx + 3u] = 0xFFu;
            }
            else
            {
                buf[idx + 0u] = 0x00u;
                buf[idx + 1u] = 0xFFu; // G (緑)
                buf[idx + 2u] = 0x00u;
                buf[idx + 3u] = 0xFFu;
            }
        }
    }

    // スレッド 1: tile[0] を GET する
    std::vector<uint8_t> result0;
    std::thread t1([&]()
    {
        PipePair hp0, hw0;
        WireChannel hostCh0(hp0.readFd,  hw0.writeFd);
        WireChannel plugCh0(hw0.readFd,  hp0.writeFd);

        // プラグイン側の書き込み
        plugCh0.WriteUint32(1u);  // drawable_id
        plugCh0.WriteUint32(0u);  // tile_num = 0 (赤タイル)
        plugCh0.WriteUint32(0u);  // shadow

        std::thread hostT([&]()
        {
            HandleTileRequest(hostCh0, ctx);
        });

        // GP_TILE_DATA を受信
        plugCh0.ReadUint32(); // TileData type
        plugCh0.ReadUint32(); // drawable_id
        plugCh0.ReadUint32(); // tile_num
        plugCh0.ReadUint32(); // shadow
        plugCh0.ReadUint32(); // bpp
        const uint32_t rw = plugCh0.ReadUint32(); // width
        const uint32_t rh = plugCh0.ReadUint32(); // height
        plugCh0.ReadUint32(); // use_shm

        const uint32_t bytes = rw * rh * 4u;
        result0.resize(bytes);
        plugCh0.ReadBytes(result0.data(), bytes);

        plugCh0.WriteUint32(static_cast<uint32_t>(GpMessageType::TileAck));
        hostT.join();
    });

    // スレッド 2: tile[1] を GET する
    std::vector<uint8_t> result1;
    std::thread t2([&]()
    {
        PipePair hp1, hw1;
        WireChannel hostCh1(hp1.readFd,  hw1.writeFd);
        WireChannel plugCh1(hw1.readFd,  hp1.writeFd);

        plugCh1.WriteUint32(1u);  // drawable_id
        plugCh1.WriteUint32(1u);  // tile_num = 1 (緑タイル)
        plugCh1.WriteUint32(0u);  // shadow

        std::thread hostT([&]()
        {
            HandleTileRequest(hostCh1, ctx);
        });

        plugCh1.ReadUint32(); // TileData
        plugCh1.ReadUint32();
        plugCh1.ReadUint32();
        plugCh1.ReadUint32();
        plugCh1.ReadUint32();
        const uint32_t rw = plugCh1.ReadUint32();
        const uint32_t rh = plugCh1.ReadUint32();
        plugCh1.ReadUint32(); // use_shm

        const uint32_t bytes = rw * rh * 4u;
        result1.resize(bytes);
        plugCh1.ReadBytes(result1.data(), bytes);

        plugCh1.WriteUint32(static_cast<uint32_t>(GpMessageType::TileAck));
        hostT.join();
    });

    t1.join();
    t2.join();

    // tile[0] は赤、tile[1] は緑であること（scratch バッファが独立していれば干渉しない）
    REQUIRE_FALSE(result0.empty());
    REQUIRE_FALSE(result1.empty());
    REQUIRE(result0[0u] == 0xFFu); // tile[0] R = 赤
    REQUIRE(result0[1u] == 0x00u); // tile[0] G
    REQUIRE(result1[0u] == 0x00u); // tile[1] R
    REQUIRE(result1[1u] == 0xFFu); // tile[1] G = 緑
}

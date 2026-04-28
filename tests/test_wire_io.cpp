/**
 * @file   test_wire_io.cpp
 * @brief  src/ipc/wire_io の単体テスト
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * WireChannel の Wire Protocol シリアライズ/デシリアライズをパイプで検証する。
 * 実際の GIMP プロセスは不要（両端を同一プロセスで操作する "loopback" テスト）。
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "../src/ipc/wire_io.h"

// ---------------------------------------------------------------------------
// テストヘルパー
// ---------------------------------------------------------------------------

namespace
{

/// pipe() / _pipe() で作成した fd ペア (read, write) を保持し RAII で閉じる
struct PipePair
{
    int readFd  = -1;
    int writeFd = -1;

    PipePair()
    {
#ifdef _WIN32
        int fds[2];
        if (::_pipe(fds, 4096, _O_BINARY) != 0)
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// 低レベルプリミティブ — 整数の Big-Endian ラウンドトリップ
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: uint32 round-trip (Big-Endian)", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteUint32(0xDEADBEEFu);
    REQUIRE(ch.ReadUint32() == 0xDEADBEEFu);

    ch.WriteUint32(0u);
    REQUIRE(ch.ReadUint32() == 0u);

    ch.WriteUint32(0xFFFFFFFFu);
    REQUIRE(ch.ReadUint32() == 0xFFFFFFFFu);
}

TEST_CASE("WireChannel: int32 round-trip", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteInt32(0);
    REQUIRE(ch.ReadInt32() == 0);

    ch.WriteInt32(-1);
    REQUIRE(ch.ReadInt32() == -1);

    ch.WriteInt32(std::numeric_limits<int32_t>::min());
    REQUIRE(ch.ReadInt32() == std::numeric_limits<int32_t>::min());

    ch.WriteInt32(std::numeric_limits<int32_t>::max());
    REQUIRE(ch.ReadInt32() == std::numeric_limits<int32_t>::max());
}

TEST_CASE("WireChannel: int64 round-trip", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteInt64(0LL);
    REQUIRE(ch.ReadInt64() == 0LL);

    ch.WriteInt64(-1LL);
    REQUIRE(ch.ReadInt64() == -1LL);

    ch.WriteInt64(INT64_MIN);
    REQUIRE(ch.ReadInt64() == INT64_MIN);

    ch.WriteInt64(INT64_MAX);
    REQUIRE(ch.ReadInt64() == INT64_MAX);
}

TEST_CASE("WireChannel: uint32 byte order is Big-Endian", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    // 0x01020304 をビッグエンディアンで書いた場合 → バイト列: 01 02 03 04
    ch.WriteUint32(0x01020304u);

    uint8_t b[4] = {};
#ifdef _WIN32
    ::_read(pipe.readFd, b, 4);
#else
    ::read(pipe.readFd, b, 4);
#endif
    REQUIRE(b[0] == 0x01u);
    REQUIRE(b[1] == 0x02u);
    REQUIRE(b[2] == 0x03u);
    REQUIRE(b[3] == 0x04u);
}

// ---------------------------------------------------------------------------
// 文字列のラウンドトリップ
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: string round-trip", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    // 通常文字列
    ch.WriteString("plug-in-gauss");
    REQUIRE(ch.ReadString() == "plug-in-gauss");

    // 空文字列 → length=0 (NULL として送出)
    ch.WriteString("");
    REQUIRE(ch.ReadString() == "");

    // ASCII 以外
    ch.WriteString("テスト");
    REQUIRE(ch.ReadString() == "テスト");
}

TEST_CASE("WireChannel: empty string is written as length=0", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteString("");

    // length フィールドが 0 で書かれていること
    uint8_t b[4] = {};
#ifdef _WIN32
    ::_read(pipe.readFd, b, 4);
#else
    ::read(pipe.readFd, b, 4);
#endif
    REQUIRE(b[0] == 0u);
    REQUIRE(b[1] == 0u);
    REQUIRE(b[2] == 0u);
    REQUIRE(b[3] == 0u);
}

TEST_CASE("WireChannel: non-empty string is written with length including NUL", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    // "abc" → length = 4 (= 3 + NUL)
    ch.WriteString("abc");

    // length フィールドを手動読み取り
    uint8_t lenBuf[4] = {};
#ifdef _WIN32
    ::_read(pipe.readFd, lenBuf, 4);
#else
    ::read(pipe.readFd, lenBuf, 4);
#endif
    const uint32_t len = (static_cast<uint32_t>(lenBuf[0]) << 24u) |
                         (static_cast<uint32_t>(lenBuf[1]) << 16u) |
                         (static_cast<uint32_t>(lenBuf[2]) <<  8u) |
                          static_cast<uint32_t>(lenBuf[3]);
    REQUIRE(len == 4u); // 3 chars + NUL

    // データ部分 ("abc\0")
    char data[4] = {};
#ifdef _WIN32
    ::_read(pipe.readFd, data, 4);
#else
    ::read(pipe.readFd, data, 4);
#endif
    REQUIRE(data[0] == 'a');
    REQUIRE(data[1] == 'b');
    REQUIRE(data[2] == 'c');
    REQUIRE(data[3] == '\0');
}

// ---------------------------------------------------------------------------
// GP_PROC_INSTALL ラウンドトリップ
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: ReadProcInstall parses minimal payload", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    // GP_PROC_INSTALL ペイロードを書く（type uint32 は dispatch 層が消費済み想定）
    // シンプルなケース: params/return_vals なし
    ch.WriteString("plug-in-test");  // name
    ch.WriteUint32(1u);              // proc_type (PLUGIN)
    ch.WriteUint32(0u);              // n_params
    ch.WriteUint32(0u);              // n_return_vals

    GpProcInstall result = ch.ReadProcInstall();

    REQUIRE(result.name == "plug-in-test");
    REQUIRE(result.procType == 1u);
    REQUIRE(result.params.empty());
    REQUIRE(result.returnVals.empty());
}

TEST_CASE("WireChannel: ReadProcInstall parses DEFAULT params", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    // 1 param (DEFAULT type): run-mode
    ch.WriteString("plug-in-blur");
    ch.WriteUint32(1u);  // proc_type
    ch.WriteUint32(1u);  // n_params = 1
    ch.WriteUint32(0u);  // n_return_vals = 0

    // GPParamDef: DEFAULT (type=0), 共通フィールドのみ、meta なし
    ch.WriteUint32(0u);              // param_def_type = DEFAULT
    ch.WriteString("gint");          // type_name
    ch.WriteString("gint");          // value_type_name
    ch.WriteString("run-mode");      // name
    ch.WriteString("Run mode");      // nick
    ch.WriteString("Interactive, non-interactive"); // blurb
    ch.WriteUint32(0u);              // flags

    GpProcInstall result = ch.ReadProcInstall();

    REQUIRE(result.name == "plug-in-blur");
    REQUIRE(result.params.size() == 1u);
    REQUIRE(result.params[0].name == "run-mode");
    REQUIRE(result.params[0].paramDefType == GpParamDefType::Default);
    REQUIRE(result.params[0].typeName == "gint");
}

TEST_CASE("WireChannel: ReadProcInstall parses INT param with meta", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteString("plug-in-sharpen");
    ch.WriteUint32(1u);  // proc_type
    ch.WriteUint32(1u);  // n_params
    ch.WriteUint32(0u);  // n_return_vals

    // GPParamDef: INT (type=1) — meta: int64 min, max, default
    ch.WriteUint32(1u);              // param_def_type = INT
    ch.WriteString("gint32");
    ch.WriteString("gint32");
    ch.WriteString("amount");
    ch.WriteString("Amount");
    ch.WriteString("Sharpen amount");
    ch.WriteUint32(0u);              // flags
    ch.WriteInt64(0LL);              // min
    ch.WriteInt64(100LL);            // max
    ch.WriteInt64(50LL);             // default

    GpProcInstall result = ch.ReadProcInstall();

    REQUIRE(result.params.size() == 1u);
    REQUIRE(result.params[0].paramDefType == GpParamDefType::Int);
    REQUIRE(result.params[0].name == "amount");
}

// ---------------------------------------------------------------------------
// GP_PROC_RUN ラウンドトリップ
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: ReadProcRun parses PDB callback with string params", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    // gimp-pdb-set-proc-menu-label(proc_name: STRING, menu_label: STRING)
    ch.WriteString("gimp-pdb-set-proc-menu-label");
    ch.WriteUint32(2u);  // n_params

    // param 0: STRING, "plug-in-blur"
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::String));
    ch.WriteString("gchararray");
    ch.WriteString("plug-in-blur");

    // param 1: STRING, "Blur..."
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::String));
    ch.WriteString("gchararray");
    ch.WriteString("Blur...");

    GpProcRunMsg msg = ch.ReadProcRun();

    REQUIRE(msg.name == "gimp-pdb-set-proc-menu-label");
    REQUIRE(msg.params.size() == 2u);
    REQUIRE(msg.params[0].paramType == GpParamType::String);
    REQUIRE(msg.params[0].stringValue == "plug-in-blur");
    REQUIRE(msg.params[1].stringValue == "Blur...");
}

TEST_CASE("WireChannel: ReadProcRun parses INT param", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteString("gimp-pdb-set-proc-sensitivity-mask");
    ch.WriteUint32(2u);

    // param 0: STRING (proc_name)
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::String));
    ch.WriteString("gchararray");
    ch.WriteString("plug-in-sharpen");

    // param 1: INT (mask)
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("gint");
    ch.WriteInt32(42);

    GpProcRunMsg msg = ch.ReadProcRun();

    REQUIRE(msg.name == "gimp-pdb-set-proc-sensitivity-mask");
    REQUIRE(msg.params[1].paramType == GpParamType::Int);
    REQUIRE(msg.params[1].intValue == 42);
}

// ---------------------------------------------------------------------------
// WriteProcReturn のフォーマット検証
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: WriteProcReturn writes correct wire format", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteProcReturn("gimp-test-proc", GIMP_PDB_SUCCESS);

    // 1. message type = GP_PROC_RETURN (=6)
    REQUIRE(ch.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));

    // 2. proc name
    REQUIRE(ch.ReadString() == "gimp-test-proc");

    // 3. n_params = 1
    REQUIRE(ch.ReadUint32() == 1u);

    // 4. param_type = GP_PARAM_TYPE_INT (=0)
    REQUIRE(ch.ReadUint32() == static_cast<uint32_t>(GpParamType::Int));

    // 5. type_name = "GimpPDBStatusType"
    REQUIRE(ch.ReadString() == "GimpPDBStatusType");

    // 6. status = GIMP_PDB_SUCCESS (=0)
    REQUIRE(ch.ReadInt32() == GIMP_PDB_SUCCESS);
}

// ---------------------------------------------------------------------------
// WriteConfig のフォーマット検証
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: WriteConfig writes correct wire format", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteConfig(GIMP_PROTOCOL_VERSION_3_2, GIMP_TILE_WIDTH, GIMP_TILE_HEIGHT);

    REQUIRE(ch.ReadUint32() == static_cast<uint32_t>(GpMessageType::Config));
    REQUIRE(ch.ReadUint32() == GIMP_PROTOCOL_VERSION_3_2); // 0x0117 = 279
    REQUIRE(ch.ReadUint32() == GIMP_TILE_WIDTH);           // 64
    REQUIRE(ch.ReadUint32() == GIMP_TILE_HEIGHT);          // 64
}

// ---------------------------------------------------------------------------
// WriteQuit のフォーマット検証
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: WriteQuit writes GP_QUIT type", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    ch.WriteQuit();
    REQUIRE(ch.ReadUint32() == static_cast<uint32_t>(GpMessageType::Quit));
}

// ---------------------------------------------------------------------------
// WireError: EOF 検出
// ---------------------------------------------------------------------------

TEST_CASE("WireChannel: ReadUint32 throws WireError on EOF", "[wire_io]")
{
    PipePair pipe;
    WireChannel ch(pipe.readFd, pipe.writeFd);

    // write 側を閉じると read 側が EOF になる
    PipePair::closefd(pipe.writeFd);

    REQUIRE_THROWS_AS(ch.ReadUint32(), WireError);
}

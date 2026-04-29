/**
 * @file   test_pdb_stubs.cpp
 * @brief  src/host/pdb_stubs の単体テスト
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * HostContext::Dispatch が GP_PROC_RUN メッセージに対して
 * 正しい GP_PROC_RETURN を WireChannel に書くことを検証する。
 * パイプを使ったループバックテスト（GIMP プロセス不要）。
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "../src/host/pdb_stubs.h"
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

/**
 * @brief HostContext::Dispatch 用テストフィクスチャ
 *
 * Dispatch が書いた GP_PROC_RETURN を readCh 経由で読み取り検証する。
 * dispatchCh の readFd = -1 は Dispatch が Write のみを行うため無害。
 */
struct DispatchFixture
{
    PipePair    pipe;
    WireChannel dispatchCh; ///< Dispatch が GP_PROC_RETURN を書く先
    WireChannel readCh;     ///< テストが GP_PROC_RETURN を読む先
    HostContext ctx;

    explicit DispatchFixture(uint32_t w = 100u, uint32_t h = 200u)
        : dispatchCh(-1, pipe.writeFd)
        , readCh(pipe.readFd, -1)
        , ctx(w, h)
    {
    }

    /// 指定プロシージャ名で Dispatch を呼ぶ
    void dispatch(const std::string& procName,
                  const std::vector<GpParam>& params = {})
    {
        GpProcRunMsg msg;
        msg.name   = procName;
        msg.params = params;
        ctx.Dispatch(msg, dispatchCh);
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// プロパティ
// ---------------------------------------------------------------------------

TEST_CASE("HostContext: Width / Height が正しく返る", "[pdb_stubs]")
{
    HostContext ctx(320u, 240u);
    REQUIRE(ctx.Width()  == 320u);
    REQUIRE(ctx.Height() == 240u);
}

// ---------------------------------------------------------------------------
// gimp-image-list — IdArray([IMAGE_ID])
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch gimp-image-list: IdArray([1])", "[pdb_stubs]")
{
    DispatchFixture f;
    f.dispatch("gimp-image-list");

    // メッセージタイプ
    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    REQUIRE(f.readCh.ReadString() == "gimp-image-list");
    REQUIRE(f.readCh.ReadUint32() == 2u); // n_params = 2

    // param[0]: status
    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpParamType::Int));
    REQUIRE(f.readCh.ReadString() == "GimpPDBStatusType");
    REQUIRE(f.readCh.ReadInt32()  == GIMP_PDB_SUCCESS);

    // param[1]: IdArray([1])
    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpParamType::IdArray));
    REQUIRE(f.readCh.ReadString() == "GimpImage");
    REQUIRE(f.readCh.ReadUint32() == 1u); // count = 1
    REQUIRE(f.readCh.ReadInt32()  == HostContext::IMAGE_ID);
}

// ---------------------------------------------------------------------------
// gimp-display-list — IdArray([]) (空リスト)
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch gimp-display-list: 空 IdArray", "[pdb_stubs]")
{
    DispatchFixture f;
    f.dispatch("gimp-display-list");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    REQUIRE(f.readCh.ReadString() == "gimp-display-list");
    REQUIRE(f.readCh.ReadUint32() == 2u);

    // param[0]: status
    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpParamType::Int));
    f.readCh.ReadString(); // "GimpPDBStatusType"
    REQUIRE(f.readCh.ReadInt32() == GIMP_PDB_SUCCESS);

    // param[1]: 空 IdArray
    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpParamType::IdArray));
    REQUIRE(f.readCh.ReadString() == "GimpDisplay");
    REQUIRE(f.readCh.ReadUint32() == 0u); // count = 0
}

// ---------------------------------------------------------------------------
// gimp-image-get-active-drawable — Int(DRAWABLE_ID)
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch gimp-image-get-active-drawable: Int(1)", "[pdb_stubs]")
{
    DispatchFixture f;
    f.dispatch("gimp-image-get-active-drawable");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    REQUIRE(f.readCh.ReadString() == "gimp-image-get-active-drawable");
    REQUIRE(f.readCh.ReadUint32() == 2u);

    // param[0]: status
    f.readCh.ReadUint32(); f.readCh.ReadString();
    REQUIRE(f.readCh.ReadInt32() == GIMP_PDB_SUCCESS);

    // param[1]: drawable_id
    f.readCh.ReadUint32(); // GpParamType::Int
    REQUIRE(f.readCh.ReadString() == "GimpDrawable");
    REQUIRE(f.readCh.ReadInt32()  == HostContext::DRAWABLE_ID);
}

// ---------------------------------------------------------------------------
// gimp-drawable-get-width / height — 幅・高さを Int で返す
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch drawable-width: Int(w)", "[pdb_stubs]")
{
    DispatchFixture f(128u, 64u);
    f.dispatch("gimp-drawable-get-width");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    f.readCh.ReadString(); // proc name
    REQUIRE(f.readCh.ReadUint32() == 2u);

    f.readCh.ReadUint32(); f.readCh.ReadString(); f.readCh.ReadInt32(); // status

    f.readCh.ReadUint32(); // param type
    REQUIRE(f.readCh.ReadString() == "gint");
    REQUIRE(f.readCh.ReadInt32()  == 128);
}

TEST_CASE("HostContext::Dispatch drawable-height: Int(h)", "[pdb_stubs]")
{
    DispatchFixture f(128u, 64u);
    f.dispatch("gimp-drawable-get-height");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    f.readCh.ReadString();
    REQUIRE(f.readCh.ReadUint32() == 2u);

    f.readCh.ReadUint32(); f.readCh.ReadString(); f.readCh.ReadInt32(); // status

    f.readCh.ReadUint32();
    REQUIRE(f.readCh.ReadString() == "gint");
    REQUIRE(f.readCh.ReadInt32()  == 64);
}

// ---------------------------------------------------------------------------
// gimp-drawable-type — RGBA = IMAGE_TYPE_RGBA
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch drawable-type: RGBA=1", "[pdb_stubs]")
{
    DispatchFixture f;
    f.dispatch("gimp-drawable-type");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    f.readCh.ReadString();
    REQUIRE(f.readCh.ReadUint32() == 2u);

    f.readCh.ReadUint32(); f.readCh.ReadString(); f.readCh.ReadInt32(); // status

    f.readCh.ReadUint32();
    REQUIRE(f.readCh.ReadString() == "GimpImageType");
    REQUIRE(f.readCh.ReadInt32()  == HostContext::IMAGE_TYPE_RGBA);
}

// ---------------------------------------------------------------------------
// gimp-drawable-has-alpha — 1 (TRUE)
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch drawable-has-alpha: 1", "[pdb_stubs]")
{
    DispatchFixture f;
    f.dispatch("gimp-drawable-has-alpha");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    f.readCh.ReadString();
    REQUIRE(f.readCh.ReadUint32() == 2u);

    f.readCh.ReadUint32(); f.readCh.ReadString(); f.readCh.ReadInt32(); // status

    f.readCh.ReadUint32();
    REQUIRE(f.readCh.ReadString() == "gboolean");
    REQUIRE(f.readCh.ReadInt32()  == 1);
}

// ---------------------------------------------------------------------------
// 未知のプロシージャ — status=SUCCESS のみ (n_params=1)
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch unknown proc: status-only SUCCESS", "[pdb_stubs]")
{
    DispatchFixture f;
    f.dispatch("gimp-some-unknown-procedure");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    REQUIRE(f.readCh.ReadString() == "gimp-some-unknown-procedure");
    REQUIRE(f.readCh.ReadUint32() == 1u); // n_params = 1 (status のみ)

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpParamType::Int));
    REQUIRE(f.readCh.ReadString() == "GimpPDBStatusType");
    REQUIRE(f.readCh.ReadInt32()  == GIMP_PDB_SUCCESS);
}

// ---------------------------------------------------------------------------
// アンダースコア形式のエイリアスも認識する
// ---------------------------------------------------------------------------

TEST_CASE("HostContext::Dispatch gimp_image_list (underscore): IdArray([1])", "[pdb_stubs]")
{
    DispatchFixture f;
    f.dispatch("gimp_image_list");

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpMessageType::ProcReturn));
    f.readCh.ReadString();
    REQUIRE(f.readCh.ReadUint32() == 2u);

    f.readCh.ReadUint32(); f.readCh.ReadString(); REQUIRE(f.readCh.ReadInt32() == GIMP_PDB_SUCCESS);

    REQUIRE(f.readCh.ReadUint32() == static_cast<uint32_t>(GpParamType::IdArray));
    f.readCh.ReadString(); // "GimpImage"
    REQUIRE(f.readCh.ReadUint32() == 1u);
    REQUIRE(f.readCh.ReadInt32()  == HostContext::IMAGE_ID);
}

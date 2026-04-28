/**
 * @file   wire_io.cpp
 * @brief  GIMP Wire Protocol I/O プリミティブと PluginSession の実装
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * scan_and_select.py の Wire I/O 関数群を C++ に移植。
 *
 * 実装の要点:
 *   - すべての整数は Big-Endian (ネットワークバイトオーダー)
 *   - 文字列: uint32(length_with_nul) + length bytes (末尾 NUL)。length=0 は NULL
 *   - メッセージ: uint32(type) + ペイロード（全体長フィールドなし）
 *   - GPParamDef の type 依存 meta は scan_and_select.py の read_param_def に準拠
 *   - GPParam の type 依存 data は scan_and_select.py の read_param_value に準拠
 */

#include "wire_io.h"

#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>      // _read, _write, _close
#else
#include <cerrno>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// WireChannel — コンストラクター
// ---------------------------------------------------------------------------

WireChannel::WireChannel(int readFd, int writeFd)
    : m_readFd(readFd)
    , m_writeFd(writeFd)
{
}

// ---------------------------------------------------------------------------
// WireChannel — 低レベル I/O ヘルパー
// ---------------------------------------------------------------------------

void WireChannel::ReadExact(void* buf, size_t n)
{
    auto* dst = static_cast<char*>(buf);
    size_t done = 0;
    while (done < n)
    {
#ifdef _WIN32
        const int got = ::_read(m_readFd, dst + done,
            static_cast<unsigned int>(n - done));
#else
        int got;
        do { got = static_cast<int>(::read(m_readFd, dst + done, n - done)); }
        while (got < 0 && errno == EINTR);
#endif
        if (got <= 0)
            throw WireError("ReadExact: EOF or I/O error");
        done += static_cast<size_t>(got);
    }
}

void WireChannel::WriteExact(const void* buf, size_t n)
{
    const auto* src = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < n)
    {
#ifdef _WIN32
        const int sent = ::_write(m_writeFd, src + done,
            static_cast<unsigned int>(n - done));
#else
        int sent;
        do { sent = static_cast<int>(::write(m_writeFd, src + done, n - done)); }
        while (sent < 0 && errno == EINTR);
#endif
        if (sent <= 0)
            throw WireError("WriteExact: I/O error");
        done += static_cast<size_t>(sent);
    }
}

// ---------------------------------------------------------------------------
// WireChannel — 型付き読み書き (Big-Endian)
// ---------------------------------------------------------------------------

uint32_t WireChannel::ReadUint32()
{
    uint8_t b[4];
    ReadExact(b, 4);
    return (static_cast<uint32_t>(b[0]) << 24u) |
           (static_cast<uint32_t>(b[1]) << 16u) |
           (static_cast<uint32_t>(b[2]) <<  8u) |
            static_cast<uint32_t>(b[3]);
}

int32_t WireChannel::ReadInt32()
{
    const uint32_t u = ReadUint32();
    int32_t result;
    std::memcpy(&result, &u, sizeof(result));
    return result;
}

int64_t WireChannel::ReadInt64()
{
    uint8_t b[8];
    ReadExact(b, 8);
    const uint64_t u =
        (static_cast<uint64_t>(b[0]) << 56u) |
        (static_cast<uint64_t>(b[1]) << 48u) |
        (static_cast<uint64_t>(b[2]) << 40u) |
        (static_cast<uint64_t>(b[3]) << 32u) |
        (static_cast<uint64_t>(b[4]) << 24u) |
        (static_cast<uint64_t>(b[5]) << 16u) |
        (static_cast<uint64_t>(b[6]) <<  8u) |
         static_cast<uint64_t>(b[7]);
    int64_t result;
    std::memcpy(&result, &u, sizeof(result));
    return result;
}

double WireChannel::ReadDouble()
{
    uint8_t b[8];
    ReadExact(b, 8);
    const uint64_t u =
        (static_cast<uint64_t>(b[0]) << 56u) |
        (static_cast<uint64_t>(b[1]) << 48u) |
        (static_cast<uint64_t>(b[2]) << 40u) |
        (static_cast<uint64_t>(b[3]) << 32u) |
        (static_cast<uint64_t>(b[4]) << 24u) |
        (static_cast<uint64_t>(b[5]) << 16u) |
        (static_cast<uint64_t>(b[6]) <<  8u) |
         static_cast<uint64_t>(b[7]);
    double result;
    std::memcpy(&result, &u, sizeof(result));
    return result;
}

std::string WireChannel::ReadString()
{
    const uint32_t len = ReadUint32();
    if (len == 0u)
        return {};
    std::string s(len, '\0');
    ReadExact(s.data(), len);
    // 末尾 NUL を除去
    if (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

void WireChannel::WriteUint32(uint32_t v)
{
    const uint8_t b[4] = {
        static_cast<uint8_t>(v >> 24u),
        static_cast<uint8_t>(v >> 16u),
        static_cast<uint8_t>(v >>  8u),
        static_cast<uint8_t>(v),
    };
    WriteExact(b, 4);
}

void WireChannel::WriteInt32(int32_t v)
{
    uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    WriteUint32(u);
}

void WireChannel::WriteInt64(int64_t v)
{
    uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    const uint8_t b[8] = {
        static_cast<uint8_t>(u >> 56u),
        static_cast<uint8_t>(u >> 48u),
        static_cast<uint8_t>(u >> 40u),
        static_cast<uint8_t>(u >> 32u),
        static_cast<uint8_t>(u >> 24u),
        static_cast<uint8_t>(u >> 16u),
        static_cast<uint8_t>(u >>  8u),
        static_cast<uint8_t>(u),
    };
    WriteExact(b, 8);
}

void WireChannel::WriteString(const std::string& s)
{
    if (s.empty())
    {
        WriteUint32(0u);
        return;
    }
    WriteUint32(static_cast<uint32_t>(s.size() + 1u)); // +1 for NUL
    WriteExact(s.data(), s.size());
    const char nul = '\0';
    WriteExact(&nul, 1);
}

// ---------------------------------------------------------------------------
// WireChannel — GeglColor 読み飛ばし
//
// フォーマット（scan_and_select.py read_gegl_color 準拠）:
//   uint32 size (<= 40)
//   size bytes        (raw BABL color data)
//   string encoding
//   uint32 icc_length
//   icc_length bytes
// ---------------------------------------------------------------------------

void WireChannel::SkipBytes(uint32_t n)
{
    std::vector<uint8_t> buf(n);
    if (n > 0u)
        ReadExact(buf.data(), n);
}

void WireChannel::SkipGeglColor()
{
    const uint32_t size = ReadUint32();
    if (size > 40u)
        throw WireError("SkipGeglColor: size too large");
    SkipBytes(size);
    ReadString();                       // encoding (discard)
    SkipBytes(ReadUint32());            // ICC profile
}

// ---------------------------------------------------------------------------
// WireChannel — GPParamDef 読み取り
//
// scan_and_select.py の read_param_def に準拠。
// type 依存 meta フィールドを読んで捨てる（struct には共通フィールドのみ保存）。
// ---------------------------------------------------------------------------

GpParamDef WireChannel::ReadParamDef()
{
    GpParamDef pd;
    pd.paramDefType  = static_cast<GpParamDefType>(ReadUint32());
    pd.typeName      = ReadString();
    pd.valueTypeName = ReadString();
    pd.name          = ReadString();
    pd.nick          = ReadString();
    pd.blurb         = ReadString();
    pd.flags         = ReadUint32();

    switch (pd.paramDefType)
    {
    case GpParamDefType::Default:
    case GpParamDefType::ExportOptions:
        // meta なし
        break;

    case GpParamDefType::Int:
        ReadInt64(); // min
        ReadInt64(); // max
        ReadInt64(); // default
        break;

    case GpParamDefType::Unit:
        ReadUint32(); // allow_pixels
        ReadUint32(); // allow_percent
        ReadUint32(); // default
        break;

    case GpParamDefType::Enum:
        ReadUint32(); // default value
        break;

    case GpParamDefType::Boolean:
        ReadUint32(); // default (TRUE/FALSE)
        break;

    case GpParamDefType::Double:
        ReadDouble(); // min
        ReadDouble(); // max
        ReadDouble(); // default
        break;

    case GpParamDefType::String:
        ReadString(); // default
        break;

    case GpParamDefType::Choice:
    {
        ReadString();                              // default_nick
        const uint32_t count = ReadUint32();
        for (uint32_t i = 0u; i < count; ++i)
        {
            ReadString();  // nick
            ReadUint32();  // id
            ReadString();  // label
            ReadString();  // help
        }
        break;
    }

    case GpParamDefType::GeglColor:
        ReadUint32();       // has_alpha
        SkipGeglColor();
        break;

    case GpParamDefType::Id:
        ReadUint32(); // none_ok
        break;

    case GpParamDefType::IdArray:
        ReadString(); // element type name
        break;

    case GpParamDefType::Resource:
        ReadUint32(); // none_ok
        ReadUint32(); // default_to_context
        ReadUint32(); // default_resource_id
        break;

    case GpParamDefType::File:
        ReadUint32(); // action
        ReadUint32(); // none_ok
        ReadString(); // default_uri
        break;

    case GpParamDefType::Curve:
        ReadUint32(); // none_ok (GIMP 3.2+)
        break;

    default:
        throw WireError("ReadParamDef: unknown GpParamDefType");
    }

    return pd;
}

// ---------------------------------------------------------------------------
// WireChannel — GPParam (ランタイム値) 読み取り
//
// scan_and_select.py の read_param_value に準拠。
// 各型のペイロードを読んで捨て、INT/DOUBLE/STRING の値のみ GpParam に保存する。
// ---------------------------------------------------------------------------

GpParam WireChannel::ReadParamValue()
{
    GpParam p;
    p.paramType = static_cast<GpParamType>(ReadUint32());
    ReadString(); // type_name (GType 名、使用しないが読み飛ばす)

    switch (p.paramType)
    {
    case GpParamType::Int:
        p.intValue = ReadInt32();
        break;

    case GpParamType::Double:
        p.doubleValue = ReadDouble();
        break;

    case GpParamType::String:
    case GpParamType::File:
        p.stringValue = ReadString();
        break;

    case GpParamType::BablFormat:
        ReadString();                    // encoding
        SkipBytes(ReadUint32());         // ICC profile data
        break;

    case GpParamType::GeglColor:
    {
        const uint32_t size = ReadUint32();
        if (size > 40u)
            throw WireError("ReadParamValue GeglColor: size too large");
        SkipBytes(size);                 // raw BABL data
        ReadString();                    // encoding
        SkipBytes(ReadUint32());         // ICC profile
        break;
    }

    case GpParamType::ColorArray:
    {
        const uint32_t count = ReadUint32();
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t size = ReadUint32();
            if (size > 40u)
                throw WireError("ReadParamValue ColorArray: size too large");
            SkipBytes(size);
            ReadString();                // encoding
            SkipBytes(ReadUint32());     // ICC profile
        }
        break;
    }

    case GpParamType::Array:
        SkipBytes(ReadUint32());         // raw byte array
        break;

    case GpParamType::Bytes:
        SkipBytes(ReadUint32());
        break;

    case GpParamType::Strv:
    {
        const uint32_t count = ReadUint32();
        for (uint32_t i = 0u; i < count; ++i)
            ReadString();
        break;
    }

    case GpParamType::IdArray:
    {
        ReadString();                    // element type name
        const uint32_t size = ReadUint32();
        for (uint32_t i = 0u; i < size; ++i)
            ReadInt32();
        break;
    }

    case GpParamType::Parasite:
    {
        const std::string parasiteName = ReadString();
        if (!parasiteName.empty())
        {
            ReadUint32();               // flags
            SkipBytes(ReadUint32());    // data
        }
        break;
    }

    case GpParamType::ExportOptions:
        // 現バージョンではペイロードなし
        break;

    case GpParamType::ParamDef:
        ReadParamDef(); // 再帰読み取り（捨てる）
        break;

    case GpParamType::ValueArray:
    {
        // 再帰: uint32 n + n * GPParam
        const uint32_t n = ReadUint32();
        for (uint32_t i = 0u; i < n; ++i)
            ReadParamValue();
        break;
    }

    case GpParamType::Curve:
        // GIMP 3.2 追加。フォーマット不明のため未対応。
        // クエリフェーズの PDB コールバックでは通常発生しない。
        throw WireError("ReadParamValue: GP_PARAM_TYPE_CURVE not yet supported");

    default:
        throw WireError("ReadParamValue: unknown GpParamType");
    }

    return p;
}

// ---------------------------------------------------------------------------
// WireChannel — メッセージ読み取り
// ---------------------------------------------------------------------------

GpProcInstall WireChannel::ReadProcInstall()
{
    GpProcInstall pi;
    pi.name     = ReadString();
    pi.procType = ReadUint32();

    const uint32_t nParams    = ReadUint32();
    const uint32_t nReturnVals = ReadUint32();

    pi.params.reserve(nParams);
    for (uint32_t i = 0u; i < nParams; ++i)
        pi.params.push_back(ReadParamDef());

    pi.returnVals.reserve(nReturnVals);
    for (uint32_t i = 0u; i < nReturnVals; ++i)
        pi.returnVals.push_back(ReadParamDef());

    return pi;
}

GpProcRunMsg WireChannel::ReadProcRun()
{
    GpProcRunMsg msg;
    msg.name = ReadString();

    const uint32_t nParams = ReadUint32();
    msg.params.reserve(nParams);
    for (uint32_t i = 0u; i < nParams; ++i)
        msg.params.push_back(ReadParamValue());

    return msg;
}

// ---------------------------------------------------------------------------
// WireChannel — メッセージ書き込み
// ---------------------------------------------------------------------------

void WireChannel::WriteProcReturn(const std::string& procName, int32_t status)
{
    WriteUint32(static_cast<uint32_t>(GpMessageType::ProcReturn));
    WriteString(procName);
    WriteUint32(1u);                                          // n_params = 1
    WriteUint32(static_cast<uint32_t>(GpParamType::Int));    // param_type = INT
    WriteString("GimpPDBStatusType");                         // type_name
    WriteInt32(status);
}

void WireChannel::WriteQuit()
{
    WriteUint32(static_cast<uint32_t>(GpMessageType::Quit));
}

void WireChannel::WriteConfig(uint32_t protocolVersion, uint32_t tileWidth, uint32_t tileHeight)
{
    WriteUint32(static_cast<uint32_t>(GpMessageType::Config));
    WriteUint32(protocolVersion);
    WriteUint32(tileWidth);
    WriteUint32(tileHeight);
}

// ---------------------------------------------------------------------------
// PluginSession — コンストラクター
// ---------------------------------------------------------------------------

PluginSession::PluginSession(
    const std::string& exePath,
    const std::string& gimpLibDir,
    PluginMode mode)
    : m_proc(SpawnPlugin(exePath, gimpLibDir, mode))
    , m_channel(m_proc.readFd, m_proc.writeFd)
    , m_mode(mode)
    , m_queryFuture(m_queryPromise.get_future().share())
{
    if (mode == PluginMode::Query)
    {
        m_worker = std::jthread(
            [this](std::stop_token st) { WorkerQueryLoop(st); });
    }
    else
    {
        m_worker = std::jthread(
            [this](std::stop_token st) { WorkerRunLoop(st); });
    }
}

// ---------------------------------------------------------------------------
// PluginSession — デストラクター
//
// 順序:
//   1. request_stop → worker ループに停止を通知
//   2. writeFd を閉じる → プラグインが EOF を見て終了し、readFd に EOF が来る
//   3. worker を join → ReadUint32 で WireError して抜けるのを待つ
//   4. readFd を閉じる
//   5. プロセス回収 / ClosePlugin
// ---------------------------------------------------------------------------

PluginSession::~PluginSession()
{
    m_worker.request_stop();

    // write 側を閉じてプラグインに EOF を通知する
    if (m_proc.writeFd >= 0)
    {
#ifdef _WIN32
        ::_close(m_proc.writeFd);
#else
        ::close(m_proc.writeFd);
#endif
        m_proc.writeFd = -1;
    }

    if (m_worker.joinable())
        m_worker.join();

    // read 側を閉じる（worker 終了後なので安全）
    if (m_proc.readFd >= 0)
    {
#ifdef _WIN32
        ::_close(m_proc.readFd);
#else
        ::close(m_proc.readFd);
#endif
        m_proc.readFd = -1;
    }

    // プロセス終了を待ち、ハンドルを解放する
    try { WaitPlugin(m_proc, 2000); }
    catch (...) { TerminatePlugin(m_proc); }
    ClosePlugin(m_proc);
}

// ---------------------------------------------------------------------------
// PluginSession — 公開 API
// ---------------------------------------------------------------------------

std::shared_future<QueryResult> PluginSession::GetQueryFuture() const
{
    return m_queryFuture;
}

std::future<void> PluginSession::RunFilter(const FilterParams& /*params*/)
{
    // step 9 で実装（tile_transfer 完成後）
    std::promise<void> p;
    p.set_exception(std::make_exception_ptr(
        std::logic_error("RunFilter: not yet implemented (step 9)")));
    return p.get_future();
}

// ---------------------------------------------------------------------------
// PluginSession — クエリフェーズ Worker
//
// scan_and_select.py の query_plugin ループを C++ に移植。
// プラグインは -query 起動後 GP_PROC_INSTALL / GP_PROC_RUN を送信して終了する。
// ---------------------------------------------------------------------------

// static
void PluginSession::ExtractPdbMeta(QueryResult& result, const GpProcRunMsg& msg)
{
    // params[0] = proc_name, params[1] = value (string 系)
    auto strAt = [&](size_t idx) -> std::string
    {
        if (idx < msg.params.size())
        {
            const auto& p = msg.params[idx];
            if (p.paramType == GpParamType::String || p.paramType == GpParamType::File)
                return p.stringValue;
        }
        return {};
    };

    if (msg.name == "gimp-pdb-set-proc-menu-label")
    {
        result.menuLabel = strAt(1);
    }
    else if (msg.name == "gimp-pdb-add-proc-menu-path")
    {
        // 最初のパスのみ保持
        if (result.menuPath.empty())
            result.menuPath = strAt(1);
    }
    else if (msg.name == "gimp-pdb-set-proc-documentation")
    {
        result.blurb = strAt(1);
        result.help  = strAt(2);
    }
    else if (msg.name == "gimp-pdb-set-proc-attribution")
    {
        result.authors      = strAt(1);
        result.copyrightStr = strAt(2);
        result.date         = strAt(3);
    }
    // その他の PDB 呼び出し（image-types, sensitivity-mask 等）は捨てる
}

void PluginSession::WorkerQueryLoop(std::stop_token stopToken)
{
    QueryResult result;

    try
    {
        while (!stopToken.stop_requested())
        {
            uint32_t msgType;
            try
            {
                msgType = m_channel.ReadUint32();
            }
            catch (const WireError&)
            {
                // EOF — プラグインがクエリ完了して正常終了した
                break;
            }

            switch (static_cast<GpMessageType>(msgType))
            {
            case GpMessageType::ProcInstall:
            {
                GpProcInstall pi = m_channel.ReadProcInstall();
                // 最初の INSTALL のみ採用（1 EXE 1 プロシージャが標準）
                if (result.procedureName.empty())
                {
                    result.procedureName = pi.name;
                    result.procType      = pi.procType;
                    result.params        = std::move(pi.params);
                    result.returnVals    = std::move(pi.returnVals);
                }
                break;
            }

            case GpMessageType::ProcRun:
            {
                GpProcRunMsg msg = m_channel.ReadProcRun();
                ExtractPdbMeta(result, msg);
                // どんな PDB コールバックにも成功で応答する（PoC の割り切り）
                m_channel.WriteProcReturn(msg.name);
                break;
            }

            case GpMessageType::HasInit:
                // ペイロードなし、読み飛ばし
                break;

            case GpMessageType::ProcUninstall:
                m_channel.ReadString(); // name のみ
                break;

            case GpMessageType::Quit:
                goto done;

            default:
                result.error = "unexpected message type during query: "
                    + std::to_string(msgType);
                goto done;
            }
        }
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
    }
    catch (...)
    {
        result.error = "unknown exception in query loop";
    }

done:
    m_queryPromise.set_value(std::move(result));
}

// ---------------------------------------------------------------------------
// PluginSession — run フェーズ Worker（step 8/9 で完成）
// ---------------------------------------------------------------------------

void PluginSession::WorkerRunLoop(std::stop_token /*stopToken*/)
{
    // step 8/9 で実装。現フェーズは placeholder のみ。
    // RunFilter() が std::logic_error を返すため、このループは呼び出されない。
}

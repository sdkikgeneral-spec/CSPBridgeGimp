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

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../host/pdb_stubs.h"
#include "../host/tile_transfer.h"

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

void WireChannel::WriteDouble(double v)
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

void WireChannel::WriteBytes(const uint8_t* data, uint32_t n)
{
    if (n > 0u)
        WriteExact(data, static_cast<size_t>(n));
}

void WireChannel::ReadBytes(uint8_t* buf, uint32_t n)
{
    if (n > 0u)
        ReadExact(buf, static_cast<size_t>(n));
}

void WireChannel::SkipBytes(uint32_t n)
{
    std::vector<uint8_t> buf(n);
    if (n > 0u)
        ReadExact(buf.data(), n);
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
    {
        // GIMP 3.2 追加 (protocol 0x0117)。可変長フォーマット（spec.md §10.4 参照）:
        //   uint32 curve_type, uint32 n_points, uint32 n_samples,
        //   double[2*n_points] points, uint32[n_points] point_types, double[n_samples] samples
        ReadUint32();                              // curve_type (捨てる)
        const uint32_t nPoints  = ReadUint32();
        const uint32_t nSamples = ReadUint32();
        SkipBytes(2u * nPoints  * 8u);            // points[]      (double × 2n)
        SkipBytes(nPoints       * 4u);            // point_types[] (uint32 × n)
        SkipBytes(nSamples      * 8u);            // samples[]     (double × n)
        break;
    }

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

// GIMP 3.2 GP_CONFIG payload — 27 fields in the order read by
// _gp_config_read (libgimpbase/gimpprotocol.c on the gimp-3-2 branch).
// protocol_version is NOT in the payload (passed via argv[2] at spawn).
void WireChannel::WriteConfig(uint32_t tileWidth, uint32_t tileHeight)
{
    auto WriteInt8 = [this](uint8_t v)
    {
        WriteExact(&v, 1);
    };

    // Minimal valid GeglColor.
    // _gimp_wire_read_gegl_color leaves icc_data NULL when icc_length=0,
    // and _gimp_config calls g_bytes_get_data(icc, ...) without a NULL check
    // → segfault. Send a 1-byte dummy ICC so the GBytes is non-NULL; babl
    // will fail to parse it and return space=NULL, which is fine.
    auto WriteWhiteGeglColor = [this]()
    {
        WriteUint32(4u);                                  // pixel size = 4 bytes
        const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        WriteBytes(white, 4u);                            // RGBA pixel
        WriteString("R'G'B'A u8");                        // BABL encoding
        WriteUint32(1u);                                  // ICC length = 1 (dummy)
        const uint8_t iccDummy = 0x00;
        WriteBytes(&iccDummy, 1u);
    };

    WriteUint32(static_cast<uint32_t>(GpMessageType::Config));

    // [1] tile_width / tile_height / shm_id (int32)
    WriteInt32(static_cast<int32_t>(tileWidth));
    WriteInt32(static_cast<int32_t>(tileHeight));
    WriteInt32(-1);  // shm_id: no shared memory in PoC

    // [2] check_size / check_type (int8) — checkerboard preview pattern config
    WriteInt8(1);
    WriteInt8(0);

    // [3] check_custom_color1 / 2 (gegl_color) — minimal valid white RGBA
    WriteWhiteGeglColor();
    WriteWhiteGeglColor();

    // [4] booleans (int8): show_help_button .. update_metadata
    WriteInt8(1);  // show_help_button
    WriteInt8(1);  // use_cpu_accel
    WriteInt8(0);  // use_opencl
    WriteInt8(1);  // export_color_profile
    WriteInt8(1);  // export_comment
    WriteInt8(1);  // export_exif
    WriteInt8(1);  // export_xmp
    WriteInt8(1);  // export_iptc
    WriteInt8(1);  // update_metadata

    // [5] default_display_id (int32)
    WriteInt32(0);

    // [6] app_name / wm_class / display_name (string)
    WriteString("CSPBridgeGimp");
    WriteString("CSPBridgeGimp");
    WriteString("");

    // [7] monitor_number / timestamp (int32)
    WriteInt32(0);
    WriteInt32(0);

    // [8] icon_theme_dir (string)
    WriteString("");

    // [9] tile_cache_size (int64) — 128 MB default
    WriteInt64(static_cast<int64_t>(128) * 1024 * 1024);

    // [10] swap_path / swap_compression (string)
    // swap_path must be a real path: gimp_file_new_for_config_path(NULL, ...) +
    // g_file_get_path() NULL-derefs in _gimp_config.
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr || *tmp == '\0')
            tmp = std::getenv("TMP");
        if (tmp == nullptr || *tmp == '\0')
            tmp = "C:\\Windows\\Temp";
        WriteString(std::string(tmp) + "\\cspbridge-gimp-swap");
    }
    WriteString("none");

    // [11] num_processors (int32)
    WriteInt32(1);
}

// ---------------------------------------------------------------------------
// PluginSession — コンストラクター
// ---------------------------------------------------------------------------

PluginSession::PluginSession(
    const std::string& exePath,
    const std::string& gimpLibDir,
    PluginMode         mode,
    HostContext*       hostContext,
    const std::string& stderrLogPath)
    : m_proc(SpawnPlugin(exePath, gimpLibDir, mode, 0x0117, stderrLogPath))
    , m_channel(m_proc.readFd, m_proc.writeFd)
    , m_mode(mode)
    , m_hostContext(hostContext)
    , m_queryFuture(m_queryPromise.get_future().share())
    , m_filterParamsFuture(m_filterParamsPromise.get_future().share())
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

std::future<void> PluginSession::RunFilter(const FilterParams& params)
{
    // ワーカースレッドにフィルターパラメーターを渡し、完了 future を返す
    m_filterParamsPromise.set_value(params);
    return m_filterDonePromise.get_future();
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
// PluginSession — run フェーズ Worker
//
// フロー（spec.md §8 / §9）:
//   1. RunFilter() がフィルターパラメーターを set するまで待機
//   2. GP_CONFIG 送信（run モード開始）
//   3. GP_PROC_RUN 送信（ホストからプラグインのフィルタープロシージャを呼び出す）
//   4. ループ: GP_PROC_RUN (PDB コールバック) / GP_TILE_REQ を受信して応答
//      GP_PROC_RETURN (フィルター完了) / GP_QUIT で抜ける
//   5. GP_QUIT 送信
//   6. m_filterDonePromise に結果を set
//
// タイルデータは step 8 ではゼロ埋めスタブ。step 9 で CSP バッファ連携を実装する。
// ---------------------------------------------------------------------------

void PluginSession::WorkerRunLoop(std::stop_token stopToken)
{
    try
    {
        // 1. RunFilter() からフィルターパラメーターが来るまで待機
        while (!stopToken.stop_requested())
        {
            if (m_filterParamsFuture.wait_for(std::chrono::milliseconds(50))
                    == std::future_status::ready)
                break;
        }
        if (stopToken.stop_requested())
            return; // デストラクターが RunFilter() 前に呼ばれた場合の正常退場

        FilterParams params = m_filterParamsFuture.get();

        // 2. GP_CONFIG — run モード開始
        // 事前チェック: プロセスがすでに死亡しているか確認（即死診断）
#ifdef _WIN32
        {
            const DWORD wr = WaitForSingleObject(m_proc.m_hProcess, 0);
            if (wr == WAIT_OBJECT_0)
            {
                DWORD exitCode = 0;
                GetExitCodeProcess(m_proc.m_hProcess, &exitCode);
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "child died before WriteConfig, exit=0x%08X", static_cast<unsigned>(exitCode));
                throw WireError(buf);
            }
        }
#endif
        try
        {
            m_channel.WriteConfig();
        }
        catch (const WireError&)
        {
            // WriteConfig 失敗 → 終了コードを取得して診断情報を付加して再投
#ifdef _WIN32
            {
                DWORD exitCode = STILL_ACTIVE;
                WaitForSingleObject(m_proc.m_hProcess, 500); // 最大 500ms 待機
                GetExitCodeProcess(m_proc.m_hProcess, &exitCode);
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "WriteConfig I/O error, child_exit=0x%08X", static_cast<unsigned>(exitCode));
                throw WireError(buf);
            }
#else
            throw;
#endif
        }

        // 3. GP_PROC_RUN でプラグインのフィルタープロシージャを呼び出す
        try
        {
            const uint32_t nParams = 3u + static_cast<uint32_t>(params.args.size());
            m_channel.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcRun));
            m_channel.WriteString(params.procedureName);
            m_channel.WriteUint32(nParams);

            // param[0]: run_mode = GIMP_RUN_NONINTERACTIVE = 1
            m_channel.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
            m_channel.WriteString("GimpRunMode");
            m_channel.WriteInt32(1);

            // param[1]: image_id
            m_channel.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
            m_channel.WriteString("GimpImage");
            m_channel.WriteInt32(HostContext::IMAGE_ID);

            // param[2]: drawables
            //
            // GP_PARAM_TYPE_ID_ARRAY のワイヤーフォーマット
            // (libgimpbase/gimpprotocol.c _gp_params_write IdArray ブランチ):
            //
            //   uint32  param_type              = GP_PARAM_TYPE_ID_ARRAY (=11)
            //   string  param->type_name        = "GimpCoreObjectArray"
            //                                     (GValue の GType 名; g_type_name(type))
            //   string  d_id_array.type_name    = "GimpItem"
            //                                     (要素 GType 名; GIMP_IS_ITEM チェックで決定。
            //                                      GimpDrawable は GimpItem のサブタイプのため
            //                                      element_type = GIMP_TYPE_ITEM = "GimpItem")
            //   uint32  size                    = 要素数
            //   int32[] data[size]              = 各要素の ID
            //
            // 出典: libgimp/gimpgpparams-body.c gimp_value_to_gp_param()
            //       GimpCoreObjectArray ブランチ
            m_channel.WriteUint32(static_cast<uint32_t>(GpParamType::IdArray));
            m_channel.WriteString("GimpCoreObjectArray"); // param->type_name (outer GType)
            m_channel.WriteString("GimpItem");            // d_id_array.type_name (element GType)
            m_channel.WriteUint32(1u);                    // size = 1
            m_channel.WriteInt32(HostContext::DRAWABLE_ID);

            // param[3..]: フィルター固有引数（type_name は PoC のため空）
            for (const auto& arg : params.args)
            {
                m_channel.WriteUint32(static_cast<uint32_t>(arg.paramType));
                m_channel.WriteString(""); // type_name: PoC 簡略化
                switch (arg.paramType)
                {
                case GpParamType::Int:
                    m_channel.WriteInt32(arg.intValue);
                    break;
                case GpParamType::Double:
                    m_channel.WriteDouble(arg.doubleValue);
                    break;
                case GpParamType::String:
                case GpParamType::File:
                    m_channel.WriteString(arg.stringValue);
                    break;
                default:
                    m_channel.WriteInt32(0); // サポート外型: 0 フォールバック
                    break;
                }
            }
        }
        catch (const WireError&)
        {
            // GP_PROC_RUN 書き込み失敗 → 子の終了コードを付加して再投
#ifdef _WIN32
            {
                DWORD exitCode = STILL_ACTIVE;
                WaitForSingleObject(m_proc.m_hProcess, 500);
                GetExitCodeProcess(m_proc.m_hProcess, &exitCode);
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "GP_PROC_RUN write failed, child_exit=0x%08X",
                    static_cast<unsigned>(exitCode));
                throw WireError(buf);
            }
#else
            throw;
#endif
        }

        // 4. PDB ディスパッチ / タイル転送ループ
        while (!stopToken.stop_requested())
        {
            uint32_t msgType;
            try
            {
                msgType = m_channel.ReadUint32();
            }
            catch (const WireError&)
            {
                break; // EOF: プラグインが終了
            }

            switch (static_cast<GpMessageType>(msgType))
            {
            case GpMessageType::ProcRun:
            {
                // プラグインからの PDB 呼び出し — HostContext に dispatch
                GpProcRunMsg msg = m_channel.ReadProcRun();
                if (m_hostContext)
                    m_hostContext->Dispatch(msg, m_channel);
                else
                    m_channel.WriteProcReturn(msg.name);
                break;
            }

            case GpMessageType::TileReq:
            {
                // タイル転送（GET / PUT）— HandleTileRequest に委譲
                // GP_TILE_REQ のメッセージタイプ uint32 はすでに消費済み。
                // drawable_id == 0xFFFFFFFF なら PUT、それ以外なら GET。
                if (m_hostContext)
                    HandleTileRequest(m_channel, *m_hostContext);
                else
                    throw WireError("TileReq received but no HostContext is set");
                break;
            }

            case GpMessageType::ProcReturn:
            {
                // フィルター完了 — 戻り値を読んで status をログ出力してから終了。
                // GP_PROC_RETURN と GP_PROC_RUN のペイロードは同一形式（GIMP Wire Protocol 仕様）:
                //   string name, uint32 n_params, GPParam[n_params]
                GpProcRunMsg result = m_channel.ReadProcRun();
                if (m_hostContext && !result.params.empty())
                {
                    char dbgBuf[128];
                    std::snprintf(dbgBuf, sizeof(dbgBuf),
                        "CSPBridge: RunLoop ProcReturn name='%s' n_params=%zu status=%d\n",
                        result.name.c_str(),
                        result.params.size(),
                        result.params[0].intValue);
                    m_hostContext->Log(dbgBuf);
                }
                goto done;
            }

            case GpMessageType::Quit:
                goto done;

            default:
                // 未知のメッセージタイプをログに記録してからエラー
                if (m_hostContext)
                {
                    char dbgBuf[64];
                    std::snprintf(dbgBuf, sizeof(dbgBuf),
                        "CSPBridge: RunLoop unknown msgType=%u\n", msgType);
                    m_hostContext->Log(dbgBuf);
                }
                throw WireError("WorkerRunLoop: unexpected message type "
                    + std::to_string(msgType));
            }
        }

done:
        m_channel.WriteQuit();
        m_filterDonePromise.set_value();
    }
    catch (...)
    {
        try { m_filterDonePromise.set_exception(std::current_exception()); }
        catch (...) {}
    }
}

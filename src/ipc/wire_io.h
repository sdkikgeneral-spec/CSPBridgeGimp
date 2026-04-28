/**
 * @file   wire_io.h
 * @brief  GIMP Wire Protocol I/O プリミティブと PluginSession
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * GIMP 3.2 プロトコル (version = 0x0117 = 279) に準拠。
 * WireChannel は fd ペアをラップして型付き Big-Endian I/O を提供する。
 * PluginSession は std::jthread で Wire Protocol 送受信を専任する。
 *
 * spec.md §8 参照。
 * scan_and_select.py の Wire I/O 関数群を C++ に移植したもの。
 */

#pragma once

#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "process.h"

// ---------------------------------------------------------------------------
// GIMP Wire Protocol 定数 — libgimpbase/gimpprotocol.h 準拠 (GIMP 3.2)
// ---------------------------------------------------------------------------

/**
 * @brief Wire Protocol メッセージタイプ
 */
enum class GpMessageType : uint32_t
{
    Quit           = 0,   ///< セッション終了（双方向）
    Config         = 1,   ///< ホスト → プラグイン: run モード開始設定
    TileReq        = 2,   ///< プラグイン → ホスト: タイル要求
    TileAck        = 3,   ///< ホスト → プラグイン: タイル受信確認
    TileData       = 4,   ///< ホスト → プラグイン: タイルデータ送信
    ProcRun        = 5,   ///< プラグイン → ホスト: PDB 呼び出し要求
    ProcReturn     = 6,   ///< ホスト → プラグイン: PDB 呼び出し応答
    TempProcRun    = 7,
    TempProcReturn = 8,
    ProcInstall    = 9,   ///< プラグイン → ホスト: プロシージャ登録
    ProcUninstall  = 10,  ///< プラグイン → ホスト: プロシージャ登録解除
    ExtensionAck   = 11,
    HasInit        = 12,  ///< プラグイン → ホスト: init ハンドラ有無通知
};

/**
 * @brief Wire Protocol パラメーター型（ランタイム）
 */
enum class GpParamType : uint32_t
{
    Int           = 0,
    Double        = 1,
    String        = 2,
    Strv          = 3,
    Bytes         = 4,
    File          = 5,
    BablFormat    = 6,
    GeglColor     = 7,
    ColorArray    = 8,
    Parasite      = 9,
    Array         = 10,
    IdArray       = 11,
    ExportOptions = 12,
    ParamDef      = 13,
    ValueArray    = 14,
    Curve         = 15,   ///< GIMP 3.2 追加 (protocol 0x0117)
};

/**
 * @brief Wire Protocol パラメーター定義型（宣言時）
 */
enum class GpParamDefType : uint32_t
{
    Default       = 0,
    Int           = 1,
    Unit          = 2,
    Enum          = 3,
    Choice        = 4,
    Boolean       = 5,
    Double        = 6,
    String        = 7,
    GeglColor     = 8,
    Id            = 9,
    IdArray       = 10,
    ExportOptions = 11,
    Resource      = 12,
    File          = 13,
    Curve         = 14,   ///< GIMP 3.2 追加 (protocol 0x0117)
};

/** @brief GIMP PDB 成功ステータス */
static constexpr int32_t GIMP_PDB_SUCCESS          = 0;
/** @brief Wire Protocol バージョン (GIMP 3.2 = 0x0117 = 279) */
static constexpr uint32_t GIMP_PROTOCOL_VERSION_3_2 = 0x0117u;
/** @brief GIMP 標準タイル幅 */
static constexpr uint32_t GIMP_TILE_WIDTH           = 64u;
/** @brief GIMP 標準タイル高さ */
static constexpr uint32_t GIMP_TILE_HEIGHT          = 64u;

// ---------------------------------------------------------------------------
// Wire Protocol エラー
// ---------------------------------------------------------------------------

/**
 * @brief Wire Protocol 読み書き中の異常（EOF・不正データ・同期喪失）
 */
class WireError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// データ型
// ---------------------------------------------------------------------------

/**
 * @brief GP_PROC_INSTALL のパラメーター定義エントリ（宣言型情報）
 */
struct GpParamDef
{
    GpParamDefType paramDefType  = GpParamDefType::Default;
    std::string    typeName;       ///< GType 名 (例: "gint")
    std::string    valueTypeName;
    std::string    name;
    std::string    nick;
    std::string    blurb;
    uint32_t       flags         = 0u;
};

/**
 * @brief GP_PROC_INSTALL から読み取ったプロシージャ定義
 */
struct GpProcInstall
{
    std::string             name;
    uint32_t                procType  = 0u;
    std::vector<GpParamDef> params;
    std::vector<GpParamDef> returnVals;
};

/**
 * @brief Wire Protocol のランタイムパラメーター 1 エントリ
 *
 * 整数・浮動小数点・文字列型の値のみ保存する。
 * その他の型（バイナリ・配列等）は読み飛ばし、対応フィールドは 0/""。
 */
struct GpParam
{
    GpParamType paramType   = GpParamType::Int;
    std::string stringValue; ///< STRING / FILE 型のみ有効
    int32_t     intValue    = 0;
    double      doubleValue = 0.0;
};

/**
 * @brief GP_PROC_RUN メッセージ（プラグインからの PDB 呼び出し）
 */
struct GpProcRunMsg
{
    std::string          name;
    std::vector<GpParam> params;
};

/**
 * @brief クエリフェーズで収集したプラグインメタデータ
 */
struct QueryResult
{
    std::string             procedureName;
    uint32_t                procType    = 0u;
    std::vector<GpParamDef> params;
    std::vector<GpParamDef> returnVals;
    std::string             menuLabel;
    std::string             menuPath;
    std::string             blurb;
    std::string             help;
    std::string             authors;
    std::string             copyrightStr; ///< "copyright" マクロとの衝突回避で末尾 Str
    std::string             date;
    std::string             error;        ///< 空なら成功

    bool IsValid() const { return !procedureName.empty() && error.empty(); }
};

/**
 * @brief フィルター実行パラメーター（run フェーズ、step 8/9 で完成）
 */
struct FilterParams
{
    std::string           procedureName; ///< GIMP プロシージャ名 ("plug-in-gauss" 等)
    std::vector<GpParam>  args;          ///< image/drawable 以外のフィルター引数
};

// ---------------------------------------------------------------------------
// WireChannel
// ---------------------------------------------------------------------------

/**
 * @brief  fd ペアをラップし GIMP Wire Protocol の Big-Endian I/O を提供するクラス
 *
 * fd の所有権は呼び出し元が保持し、WireChannel は借用のみ。
 * テスト時は pipe() / _pipe() で生成した fd ペアをそのまま渡せる。
 *
 * spec.md §8 参照。scan_and_select.py の Wire I/O 関数群を C++ 移植。
 */
class WireChannel
{
public:
    /**
     * @brief  コンストラクター
     * @param  readFd   ホストがプラグインから読む fd（親側）
     * @param  writeFd  ホストがプラグインへ書く fd（親側）
     */
    WireChannel(int readFd, int writeFd);

    // -----------------------------------------------------------------------
    // 低レベルプリミティブ（テストから直接呼び出し可）
    // -----------------------------------------------------------------------

    /** @brief  Big-Endian uint32 を読む。EOF/エラー時は WireError を投げる */
    uint32_t ReadUint32();
    /** @brief  Big-Endian int32 を読む */
    int32_t  ReadInt32();
    /** @brief  Big-Endian int64 を読む */
    int64_t  ReadInt64();
    /** @brief  Big-Endian IEEE 754 double を読む */
    double   ReadDouble();
    /**
     * @brief  Wire Protocol 文字列を読む
     *
     * フォーマット: uint32(length_with_nul) + length bytes (末尾 NUL)。
     * length == 0 は NULL を意味し、空文字列 "" として返す。
     */
    std::string ReadString();

    /** @brief  Big-Endian uint32 を書く */
    void WriteUint32(uint32_t v);
    /** @brief  Big-Endian int32 を書く */
    void WriteInt32(int32_t v);
    /** @brief  Big-Endian int64 を書く */
    void WriteInt64(int64_t v);
    /**
     * @brief  Wire Protocol 文字列を書く
     *
     * s が空なら length=0 (NULL) として送出する。
     * 非空の場合は length = s.size() + 1 (NUL 含む) で送出。
     */
    void WriteString(const std::string& s);

    // -----------------------------------------------------------------------
    // メッセージ読み取り（message type の uint32 は呼び出し前に消費済みであること）
    // -----------------------------------------------------------------------

    /** @brief  GP_PROC_INSTALL ペイロードを読む */
    GpProcInstall ReadProcInstall();

    /** @brief  GP_PROC_RUN ペイロードを読む */
    GpProcRunMsg  ReadProcRun();

    // -----------------------------------------------------------------------
    // メッセージ書き込み（message type を含む完全なメッセージを書く）
    // -----------------------------------------------------------------------

    /**
     * @brief  GP_PROC_RETURN を書く（PDB コールバックへの成功応答）
     *
     * ペイロード形式（scan_and_select.py の write_proc_return 準拠）:
     *   uint32 GP_PROC_RETURN (=6)
     *   string proc_name
     *   uint32 n_params (=1)
     *   uint32 GP_PARAM_TYPE_INT (=0)
     *   string "GimpPDBStatusType"
     *   int32  status
     *
     * @param  procName  呼び出されたプロシージャ名
     * @param  status    PDB ステータスコード（GIMP_PDB_SUCCESS = 0 が通常値）
     */
    void WriteProcReturn(const std::string& procName, int32_t status = GIMP_PDB_SUCCESS);

    /** @brief  GP_QUIT を書く（ペイロードなし）*/
    void WriteQuit();

    /**
     * @brief  GP_CONFIG を書く（run フェーズ開始時にホストが送る）
     * @param  protocolVersion  Wire Protocol バージョン（通常 0x0117 = 279）
     * @param  tileWidth        タイル幅（通常 64）
     * @param  tileHeight       タイル高さ（通常 64）
     */
    void WriteConfig(
        uint32_t protocolVersion = GIMP_PROTOCOL_VERSION_3_2,
        uint32_t tileWidth       = GIMP_TILE_WIDTH,
        uint32_t tileHeight      = GIMP_TILE_HEIGHT);

    int ReadFd()  const { return m_readFd; }
    int WriteFd() const { return m_writeFd; }

private:
    int m_readFd;
    int m_writeFd;

    void ReadExact(void* buf, size_t n);
    void WriteExact(const void* buf, size_t n);

    GpParamDef ReadParamDef();
    GpParam    ReadParamValue();
    void       SkipGeglColor();
    void       SkipBytes(uint32_t n);
};

// ---------------------------------------------------------------------------
// PluginSession
// ---------------------------------------------------------------------------

/**
 * @brief GIMP プラグイン 1 プロセスとの通信セッションを管理するクラス
 *
 * 子プロセス 1 本につき 1 インスタンスを生成し、std::jthread で
 * Wire Protocol の送受信を専任する。spec.md §8 参照。
 *
 * Query モード:
 *   - 子プロセスを -query で起動、GP_PROC_INSTALL / GP_PROC_RUN を処理
 *   - GetQueryFuture() で QueryResult を取得できる
 *
 * Run モード:
 *   - 子プロセスを -run で起動し、RunFilter() でフィルター実行を依頼
 *   - タイル転送は step 9 で完成
 */
class PluginSession
{
public:
    /**
     * @brief  コンストラクター。子プロセスを起動してスレッドを開始する
     * @param  exePath     GIMP プラグイン EXE のフルパス
     * @param  gimpLibDir  libgimp-3.0-0.dll 等があるディレクトリ
     * @param  mode        Query / Run（デフォルト Run）
     * @throws std::runtime_error プロセス起動に失敗した場合
     */
    explicit PluginSession(
        const std::string& exePath,
        const std::string& gimpLibDir,
        PluginMode mode = PluginMode::Run);

    /**
     * @brief  クエリフェーズ完了を待つ Future を返す（Query モード専用）
     *
     * QueryResult::IsValid() == false の場合は QueryResult::error にエラー理由が入る。
     */
    std::shared_future<QueryResult> GetQueryFuture() const;

    /**
     * @brief  フィルターを非同期実行する（Run モード専用）
     *
     * @param  params  フィルターパラメーター
     * @return 処理完了を通知する std::future
     * @note   タイル転送は step 9 で実装。現 PoC では std::logic_error を返す stub
     */
    std::future<void> RunFilter(const FilterParams& params);

    /** @brief  デストラクター。worker スレッドを停止し fd・プロセスを解放する */
    ~PluginSession();

    PluginSession(const PluginSession&)            = delete;
    PluginSession& operator=(const PluginSession&) = delete;

private:
    PluginProcess m_proc;
    WireChannel   m_channel;
    PluginMode    m_mode;

    std::promise<QueryResult>       m_queryPromise;
    std::shared_future<QueryResult> m_queryFuture;

    std::jthread m_worker;

    void WorkerQueryLoop(std::stop_token stopToken);
    void WorkerRunLoop(std::stop_token stopToken);

    static void ExtractPdbMeta(QueryResult& result, const GpProcRunMsg& msg);
};

/**
 * @file   pdb_stubs.cpp
 * @brief  HostContext — run フェーズ最小 PDB スタブの実装
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * 各 PDB プロシージャへの応答は Wire Protocol の GP_PROC_RETURN 形式
 * （spec.md §5.4 / §9）に準拠。タイル転送は step 9 で実装する。
 */

#include "pdb_stubs.h"

#include <cstdio>
#include <vector>

// ---------------------------------------------------------------------------
// HostContext — コンストラクター / プロパティ
// ---------------------------------------------------------------------------

HostContext::HostContext(uint32_t width, uint32_t height)
    : m_width(width)
    , m_height(height)
    , m_rgbaBuffer(static_cast<size_t>(width) * height * 4u, 0u)
{
}

void HostContext::SetLogCallback(std::function<void(const char*)> fn)
{
    m_logFn = std::move(fn);
}

void HostContext::Log(const char* msg) const
{
    if (m_logFn)
        m_logFn(msg);
}

uint32_t HostContext::Width() const
{
    std::shared_lock lock(m_mutex);
    return m_width;
}

uint32_t HostContext::Height() const
{
    std::shared_lock lock(m_mutex);
    return m_height;
}

const uint8_t* HostContext::RgbaData() const
{
    return m_rgbaBuffer.data();
}

uint8_t* HostContext::RgbaData()
{
    return m_rgbaBuffer.data();
}

std::shared_mutex& HostContext::Mutex() const
{
    return m_mutex;
}

// ---------------------------------------------------------------------------
// 内部ヘルパー — GP_PROC_RETURN に値を書く
//
// 各応答形式（spec.md §5.4 / §9）:
//   status のみ  : WriteProcReturn(name) — n_params=1, status=SUCCESS
//   status + Int : n_params=2, param[0]=status, param[1]=Int(value)
//   status + IdArray: n_params=2, param[0]=status, param[1]=IdArray(ids)
// ---------------------------------------------------------------------------

/** @brief GP_PROC_RETURN に status=SUCCESS + Int(value) を書く */
static void WriteIntReturn(
    WireChannel&       ch,
    const std::string& procName,
    const std::string& retTypeName,
    int32_t            value)
{
    ch.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcReturn));
    ch.WriteString(procName);
    ch.WriteUint32(2u);
    // param[0]: status
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpPDBStatusType");
    ch.WriteInt32(GIMP_PDB_SUCCESS);
    // param[1]: 戻り値
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString(retTypeName);
    ch.WriteInt32(value);
}

/**
 * @brief GP_PROC_RETURN に status=SUCCESS + 複数 Int を書く
 *
 * gimp-drawable-mask-intersect など、複数の整数を返すプロシージャ向け。
 */
static void WriteMultiIntReturn(
    WireChannel&                ch,
    const std::string&          procName,
    const std::vector<std::pair<std::string, int32_t>>& retVals)
{
    ch.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcReturn));
    ch.WriteString(procName);
    ch.WriteUint32(static_cast<uint32_t>(1u + retVals.size()));
    // param[0]: status
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpPDBStatusType");
    ch.WriteInt32(GIMP_PDB_SUCCESS);
    // param[1..]: 各戻り値
    for (const auto& [typeName, val] : retVals)
    {
        ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
        ch.WriteString(typeName);
        ch.WriteInt32(val);
    }
}

/**
 * @brief GP_PROC_RETURN に status=SUCCESS + GeglColor を書く
 *
 * gimp-context-get-foreground/background などのカラー返却プロシージャ向け。
 * 色データは "R'G'B'A u8" (sRGB + alpha, 8bpc) フォーマットで送出する。
 */
static void WriteGeglColorReturn(
    WireChannel&       ch,
    const std::string& procName,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    ch.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcReturn));
    ch.WriteString(procName);
    ch.WriteUint32(2u);
    // param[0]: status
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpPDBStatusType");
    ch.WriteInt32(GIMP_PDB_SUCCESS);
    // param[1]: GeglColor
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::GeglColor));
    ch.WriteString("GeglColor");
    ch.WriteUint32(4u);  // raw BABL data size = 4 bytes (R, G, B, A u8)
    const uint8_t colorData[4] = { r, g, b, a };
    ch.WriteBytes(colorData, 4u);
    ch.WriteString("R'G'B'A u8");  // BABL encoding
    ch.WriteUint32(0u);             // ICC profile length = 0
}

/**
 * @brief GP_PROC_RETURN に status=SUCCESS + String を書く
 *
 * gimp-drawable-get-format は Babl encoding 文字列のみを返す PDB。
 * libgimp 側の `gimp_drawable_get_format` ラッパーが encoding + color profile
 * を組み合わせて Babl format を生成する（gimpdrawable.c L340 参照）。
 */
static void WriteStringReturn(
    WireChannel&       ch,
    const std::string& procName,
    const std::string& value)
{
    ch.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcReturn));
    ch.WriteString(procName);
    ch.WriteUint32(2u);
    // param[0]: status
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpPDBStatusType");
    ch.WriteInt32(GIMP_PDB_SUCCESS);
    // param[1]: 戻り値文字列
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::String));
    ch.WriteString("gchararray");
    ch.WriteString(value);
}

/**
 * @brief GP_PROC_RETURN に status のみを書く（任意ステータス）
 *
 * gimp-image-get-color-profile のように戻り値があるが「未設定」を伝えたい場合に使用。
 * status != GIMP_PDB_SUCCESS にすると、libgimp 側ラッパーは戻り値読み取りをスキップする
 * （`if (GIMP_VALUES_GET_ENUM(rv,0) == SUCCESS) ...` パターン）。
 */
static void WriteStatusOnly(
    WireChannel&       ch,
    const std::string& procName,
    int32_t            status)
{
    ch.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcReturn));
    ch.WriteString(procName);
    ch.WriteUint32(1u);
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpPDBStatusType");
    ch.WriteInt32(status);
}

/** @brief GP_PROC_RETURN に status=SUCCESS + IdArray(ids) を書く */
static void WriteIdArrayReturn(
    WireChannel&                ch,
    const std::string&          procName,
    const std::string&          elemTypeName,
    const std::vector<int32_t>& ids)
{
    ch.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcReturn));
    ch.WriteString(procName);
    ch.WriteUint32(2u);
    // param[0]: status
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpPDBStatusType");
    ch.WriteInt32(GIMP_PDB_SUCCESS);
    // param[1]: IdArray
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::IdArray));
    ch.WriteString(elemTypeName);
    ch.WriteUint32(static_cast<uint32_t>(ids.size()));
    for (int32_t id : ids)
        ch.WriteInt32(id);
}

// ---------------------------------------------------------------------------
// HostContext::Dispatch
// ---------------------------------------------------------------------------

void HostContext::Dispatch(const GpProcRunMsg& msg, WireChannel& channel) const
{
    // 読み取り専用アクセスのみ (m_width / m_height) のため shared_lock
    std::shared_lock lock(m_mutex);

    const std::string& name = msg.name;

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "CSPBridge: PDB Dispatch '%s'\n", name.c_str());
        Log(buf);
    }

    // --- image 系 ---
    if (name == "gimp-image-list" || name == "gimp_image_list")
    {
        WriteIdArrayReturn(channel, name, "GimpImage", { IMAGE_ID });
    }
    else if (name == "gimp-display-list" || name == "gimp_display_list")
    {
        WriteIdArrayReturn(channel, name, "GimpDisplay", {});
    }
    // --- drawable 取得 ---
    else if (   name == "gimp-image-get-active-drawable"
             || name == "gimp_image_get_active_drawable"
             || name == "gimp-drawable-get"
             || name == "gimp_drawable_get")
    {
        WriteIntReturn(channel, name, "GimpDrawable", DRAWABLE_ID);
    }
    // --- drawable メタデータ ---
    else if (   name == "gimp-drawable-get-width"
             || name == "gimp_drawable_get_width"
             || name == "gimp-drawable-width"
             || name == "gimp_drawable_width")
    {
        WriteIntReturn(channel, name, "gint", static_cast<int32_t>(m_width));
    }
    else if (   name == "gimp-drawable-get-height"
             || name == "gimp_drawable_get_height"
             || name == "gimp-drawable-height"
             || name == "gimp_drawable_height")
    {
        WriteIntReturn(channel, name, "gint", static_cast<int32_t>(m_height));
    }
    else if (   name == "gimp-drawable-type"
             || name == "gimp_drawable_type"
             || name == "gimp-drawable-get-image-type"
             || name == "gimp_drawable_get_image_type")
    {
        WriteIntReturn(channel, name, "GimpImageType", IMAGE_TYPE_RGBA);
    }
    else if (   name == "gimp-drawable-has-alpha"
             || name == "gimp_drawable_has_alpha")
    {
        WriteIntReturn(channel, name, "gboolean", 1);
    }
    // --- drawable のカラーモード判定 ---
    //
    // checkerboard.c が描画前に gimp_drawable_is_rgb() / gimp_drawable_is_gray()
    // を呼んで色空間を確認する。RGBA バッファを扱うので is-rgb=TRUE / is-gray=FALSE。
    else if (   name == "gimp-drawable-is-rgb"
             || name == "gimp_drawable_is_rgb")
    {
        WriteIntReturn(channel, name, "gboolean", 1);
    }
    else if (   name == "gimp-drawable-is-gray"
             || name == "gimp_drawable_is_gray")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-drawable-is-indexed"
             || name == "gimp_drawable_is_indexed")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    // --- drawable の Babl format 文字列 ---
    //
    // libgimp の `gimp_drawable_get_format` は
    //   1. _gimp_drawable_get_format (この PDB) → encoding 文字列を取得
    //   2. gimp-item-get-image → 関連 image を取得
    //   3. gimp_item_is_layer (型チェック、PDB ではない)
    //   4. gimp-image-get-color-profile → ICC profile (NULL 可)
    //   5. babl_format_with_space(encoding, space) で組み立て
    // RGBA バッファを扱うため "R'G'B'A u8" を返す。
    else if (   name == "gimp-drawable-get-format"
             || name == "gimp_drawable_get_format")
    {
        WriteStringReturn(channel, name, "R'G'B'A u8");
    }
    // --- drawable から image を逆引き ---
    //
    // gimp_item_get_image() が呼ぶ。我々は IMAGE_ID 1 つしか持たないため常に 1 を返す。
    else if (   name == "gimp-item-get-image"
             || name == "gimp_item_get_image")
    {
        WriteIntReturn(channel, name, "GimpImage", IMAGE_ID);
    }
    // --- 画像のカラープロファイル ---
    //
    // 我々は ICC profile を持たないため status != SUCCESS で「プロファイル未設定」を伝える。
    // libgimp 側 `_gimp_image_get_color_profile` は status==SUCCESS の時のみ
    // bytes を読むため、EXECUTION_ERROR を返せば NULL profile として扱われ
    // gimp_drawable_get_format は babl_format_with_space(encoding, NULL) で
    // デフォルト sRGB space を使う。
    else if (   name == "gimp-image-get-color-profile"
             || name == "gimp_image_get_color_profile"
             || name == "gimp-image-get-effective-color-profile"
             || name == "gimp_image_get_effective_color_profile")
    {
        WriteStatusOnly(channel, name, GIMP_PDB_EXECUTION_ERROR);
    }
    // --- drawable の bpp ---
    else if (   name == "gimp-drawable-get-bpp"
             || name == "gimp_drawable_get_bpp"
             || name == "gimp-drawable-bpp"
             || name == "gimp_drawable_bpp")
    {
        WriteIntReturn(channel, name, "gint", 4);  // RGBA = 4 bytes/pixel
    }
    // --- 選択領域とのマスク交差
    //
    // checkerboard.c が最初に呼ぶ。FALSE を返すと plugin が即座に
    // GIMP_PDB_SUCCESS でリターンし、タイル転送が一切行われない。
    // 選択なし（全選択）として drawable 全体の矩形を返す。
    // ---
    else if (   name == "gimp-drawable-mask-intersect"
             || name == "gimp_drawable_mask_intersect")
    {
        WriteMultiIntReturn(channel, name, {
            { "gboolean", 1                          },  // non_empty = TRUE
            { "gint",     0                          },  // x
            { "gint",     0                          },  // y
            { "gint",     static_cast<int32_t>(m_width)  },  // width
            { "gint",     static_cast<int32_t>(m_height) },  // height
        });
    }
    // --- コンテキストカラー（FG / BG）
    //
    // psychobilly=FALSE の checkerboard は FG(黒) / BG(白) で市松模様を描画する。
    // NULL カラーが返ると GEGL がゼロ（透明黒）で描画してしまうため
    // 有効な GeglColor を返す必要がある。
    // ---
    else if (   name == "gimp-context-get-foreground"
             || name == "gimp_context_get_foreground")
    {
        WriteGeglColorReturn(channel, name, 0, 0, 0, 255);  // 黒・不透明
    }
    else if (   name == "gimp-context-get-background"
             || name == "gimp_context_get_background")
    {
        WriteGeglColorReturn(channel, name, 255, 255, 255, 255);  // 白・不透明
    }
    // --- アイテム / drawable / image ID 有効性チェック ---
    //
    // gimp_drawable_get_buffer() が内部で gimp_item_is_valid() →
    // gimp_item_id_is_valid() → PDB "gimp-item-id-is-valid" を呼ぶ。
    // FALSE が返ると buffer = NULL → GEGL タイル PUT 不発生 → 描画されない。
    // shadow_buffer が NULL の場合、GEGL が NULL デリファレンスしてクラッシュ
    // することもある（断続的な WriteExact I/O error の原因）。
    else if (   name == "gimp-item-id-is-valid"
             || name == "gimp_item_id_is_valid")
    {
        WriteIntReturn(channel, name, "gboolean", 1);
    }
    else if (   name == "gimp-drawable-id-is-valid"
             || name == "gimp_drawable_id_is_valid")
    {
        WriteIntReturn(channel, name, "gboolean", 1);
    }
    else if (   name == "gimp-image-id-is-valid"
             || name == "gimp_image_id_is_valid")
    {
        WriteIntReturn(channel, name, "gboolean", 1);
    }
    // --- アイテム種別判定 ---
    //
    // _gimp_plug_in_get_item が drawable ID の GType を決定するために
    // これらを連続的に呼ぶ。戻り値は n_params=2（status + gboolean）。
    // n_params=1 だと param[1] 参照で SIGSEGV になる。
    // DRAWABLE_ID は通常レイヤーなので gimp-item-id-is-layer / is-drawable のみ TRUE。
    else if (   name == "gimp-item-id-is-layer"
             || name == "gimp_item_id_is_layer")
    {
        WriteIntReturn(channel, name, "gboolean", 1);  // drawable = layer
    }
    else if (   name == "gimp-item-id-is-drawable"
             || name == "gimp_item_id_is_drawable")
    {
        WriteIntReturn(channel, name, "gboolean", 1);
    }
    else if (   name == "gimp-item-id-is-text-layer"
             || name == "gimp_item_id_is_text_layer")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-item-id-is-layer-mask"
             || name == "gimp_item_id_is_layer_mask")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-item-id-is-channel"
             || name == "gimp_item_id_is_channel")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-item-id-is-selection"
             || name == "gimp_item_id_is_selection")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-item-id-is-path"
             || name == "gimp_item_id_is_path"
             || name == "gimp-item-id-is-vectors"
             || name == "gimp_item_id_is_vectors")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-item-id-is-vector-layer"
             || name == "gimp_item_id_is_vector_layer")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-item-id-is-group-layer"
             || name == "gimp_item_id_is_group_layer")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    else if (   name == "gimp-item-id-is-link-layer"
             || name == "gimp_item_id_is_link_layer")
    {
        WriteIntReturn(channel, name, "gboolean", 0);
    }
    // --- 未知のプロシージャ: status=SUCCESS のみ ---
    else
    {
        channel.WriteProcReturn(name);
    }
}

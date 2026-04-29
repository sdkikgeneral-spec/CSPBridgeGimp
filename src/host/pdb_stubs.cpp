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

#include <vector>

// ---------------------------------------------------------------------------
// HostContext — コンストラクター / プロパティ
// ---------------------------------------------------------------------------

HostContext::HostContext(uint32_t width, uint32_t height)
    : m_width(width)
    , m_height(height)
{
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
    // --- 未知のプロシージャ: status=SUCCESS のみ ---
    else
    {
        channel.WriteProcReturn(name);
    }
}

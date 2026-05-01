/**
 * @file   pdb_stubs.h
 * @brief  run フェーズで必要な最小 PDB スタブ — HostContext クラスの定義
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * HostContext は CSP 画像バッファのメタデータ（幅・高さ等）を保持し、
 * GIMP プラグインからの GP_PROC_RUN (PDB 呼び出し) を解釈して
 * 適切な GP_PROC_RETURN を返す。spec.md §9 参照。
 *
 * std::shared_mutex で保護されており、複数 PluginSession スレッドが
 * 同一 HostContext を参照するシナリオ（step 11 以降）でもスレッドセーフ。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "../ipc/wire_io.h"

// ---------------------------------------------------------------------------
// HostContext
// ---------------------------------------------------------------------------

/**
 * @brief ホスト側の画像バッファ情報を保持し、PDB スタブを提供するクラス
 *
 * GIMP プラグインが GP_PROC_RUN で呼び出す PDB プロシージャに対し、
 * CSP バッファのメタデータ（幅・高さ・タイプ）を元に応答する。
 *
 * CSP バッファへの実データアクセス（タイル転送）は tile_transfer.cpp で実装する。
 * spec.md §9 参照。
 */
class HostContext
{
public:
    /** @brief ダミー image ID（run フェーズでは常に 1） */
    static constexpr int32_t IMAGE_ID        = 1;
    /** @brief ダミー drawable ID（run フェーズでは常に 1） */
    static constexpr int32_t DRAWABLE_ID     = 1;
    /** @brief GIMP_IMAGE_TYPE_RGBA = 1 (libgimpbase/gimpbasetypes.h) */
    static constexpr int32_t IMAGE_TYPE_RGBA = 1;

    /**
     * @brief  コンストラクター
     * @param  width   対象画像の幅 (px)
     * @param  height  対象画像の高さ (px)
     *
     * RGBA バッファを width * height * 4 バイトでゼロ初期化する。
     */
    explicit HostContext(uint32_t width, uint32_t height);

    /**
     * @brief  GP_PROC_RUN メッセージに応じた GP_PROC_RETURN を channel に書く
     *
     * 認識したプロシージャ名には適切な戻り値を返し、
     * 未知のプロシージャには status=GIMP_PDB_SUCCESS のみを返す。
     *
     * @param  msg      読み取り済みの GpProcRunMsg
     * @param  channel  書き込み先 WireChannel（ProcReturn を書く）
     */
    void Dispatch(const GpProcRunMsg& msg, WireChannel& channel) const;

    /**
     * @brief  デバッグログコールバックを設定する
     * @param  fn  ログ出力関数（nullptr で無効化）
     */
    void SetLogCallback(std::function<void(const char*)> fn);

    /** @brief  ログコールバックにメッセージを送る（wire_io 等の外部モジュールから使用） */
    void Log(const char* msg) const;

    /** @brief 幅 (px) を返す */
    uint32_t Width()  const;
    /** @brief 高さ (px) を返す */
    uint32_t Height() const;

    /**
     * @brief  RGBA バッファへの const ポインターを返す
     *
     * 呼び出し元は m_mutex を shared_lock で保護した上で使用すること。
     * @return width * height * 4 バイトのバッファ先頭ポインター
     */
    const uint8_t* RgbaData() const;

    /**
     * @brief  RGBA バッファへの非 const ポインターを返す
     *
     * 呼び出し元は m_mutex を unique_lock で保護した上で使用すること。
     * @return width * height * 4 バイトのバッファ先頭ポインター
     */
    uint8_t* RgbaData();

    /**
     * @brief  共有ミューテックスへの参照を返す
     *
     * tile_transfer の HandleTileRequest が shared_lock / unique_lock で
     * バッファアクセスを保護するために使用する。
     * @return mutable 参照（const HostContext からも取得可能）
     */
    std::shared_mutex& Mutex() const;

private:
    mutable std::shared_mutex           m_mutex;
    uint32_t                            m_width;
    uint32_t                            m_height;
    std::vector<uint8_t>                m_rgbaBuffer; ///< width * height * 4 バイト、ゼロ初期化
    std::function<void(const char*)>    m_logFn;
};

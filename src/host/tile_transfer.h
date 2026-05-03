/**
 * @file   tile_transfer.h
 * @brief  GIMP タイル転送ハンドラー — GP_TILE_REQ の GET / PUT 処理
 * @author CSPBridgeGimp
 * @date   2026-04-29
 *
 * GIMP Wire Protocol のタイル転送シーケンスを実装する。
 * プラグインからの GP_TILE_REQ を受け取り、drawable_id に応じて
 * GET（ホスト → プラグイン）または PUT（プラグイン → ホスト）を処理する。
 *
 * Wire Protocol シーケンス（libgimpbase/gimpprotocol.c 確認済み）:
 *
 *   GET (drawable_id != 0xFFFFFFFF):
 *     plugin → host : GP_TILE_REQ  { drawable_id, tile_num, shadow }
 *     host → plugin : GP_TILE_DATA { drawable_id, tile_num, shadow, bpp, width, height,
 *                                    use_shm=0, pixel_data[w*h*bpp] }
 *     plugin → host : GP_TILE_ACK  { ペイロードなし }
 *
 *   PUT (drawable_id == 0xFFFFFFFF すなわち -1):
 *     plugin → host : GP_TILE_REQ  { 0xFFFFFFFF, tile_num, shadow }
 *     host → plugin : GP_TILE_DATA { 0xFFFFFFFF, tile_num, shadow, bpp, width, height,
 *                                    use_shm=0 }  ← pixel_data なし（プロンプト）
 *     plugin → host : GP_TILE_DATA { drawable_id, tile_num, shadow, bpp, w, h,
 *                                    use_shm=0, pixel_data[w*h*bpp] }
 *     host → plugin : GP_TILE_ACK  { ペイロードなし }
 *
 * spec.md §10 参照。
 */

#pragma once

#include <cstdint>

#include "../ipc/wire_io.h"

// ---------------------------------------------------------------------------
// 前方宣言
// ---------------------------------------------------------------------------

class HostContext;

// ---------------------------------------------------------------------------
// タイルインデックス計算ユーティリティ（テスト可能な純粋関数）
// ---------------------------------------------------------------------------

/**
 * @brief  ピクセル座標 (x, y) からタイルインデックスを計算する
 *
 * GIMP タイル番号 = (y / 64) * tiles_per_row + (x / 64)。
 * ピクセル座標ではなく、タイルの左上ピクセル座標 (tile_x * 64, tile_y * 64)
 * を渡すこと。端数は切り捨て。
 *
 * @param  x           ピクセル x 座標（タイル左上）
 * @param  y           ピクセル y 座標（タイル左上）
 * @param  imageWidth  画像幅 (px)
 * @return タイルインデックス (0-based)
 */
uint32_t TileIndex(uint32_t x, uint32_t y, uint32_t imageWidth);

/**
 * @brief  タイル領域の幅を計算する（端タイルのクリッピング）
 *
 * x が端のタイル境界を越えている場合は imageWidth - x を返す。
 * それ以外は GIMP_TILE_WIDTH (64) を返す。
 *
 * @param  x           タイル左上の x 座標 (px)
 * @param  imageWidth  画像幅 (px)
 * @return タイル幅 (px)、最大 64
 */
uint32_t TileRegionWidth(uint32_t x, uint32_t imageWidth);

/**
 * @brief  タイル領域の高さを計算する（端タイルのクリッピング）
 *
 * y が端のタイル境界を越えている場合は imageHeight - y を返す。
 * それ以外は GIMP_TILE_HEIGHT (64) を返す。
 *
 * @param  y            タイル左上の y 座標 (px)
 * @param  imageHeight  画像高さ (px)
 * @return タイル高さ (px)、最大 64
 */
uint32_t TileRegionHeight(uint32_t y, uint32_t imageHeight);

// ---------------------------------------------------------------------------
// タイル転送統合ハンドラー
// ---------------------------------------------------------------------------

/**
 * @brief  GP_TILE_REQ ペイロードを読み取り、GET または PUT を処理する
 *
 * GP_TILE_REQ メッセージタイプの uint32 はすでに読み取り済みであること。
 * drawable_id が 0xFFFFFFFF（= -1）の場合は PUT、それ以外は GET として処理する。
 *
 * スレッド安全性:
 *   - GET: ctx.m_mutex を shared_lock で取得して RGBA バッファを読む
 *   - PUT: ctx.m_mutex を unique_lock で取得して RGBA バッファに書く
 *   - スレッドローカルの scratch バッファを使用するため、複数スレッドから
 *     同時に呼び出しても互いに干渉しない
 *
 * @param  channel  プラグインとの通信チャネル
 * @param  ctx      HostContext（幅・高さ・RGBA バッファを提供）
 * @throws WireError  Wire Protocol エラー（EOF・不正データ）
 */
void HandleTileRequest(WireChannel& channel, HostContext& ctx);

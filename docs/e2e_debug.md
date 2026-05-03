# E2E デバッグノート

最終更新: 2026-05-03

## 現在の状況

**`test_checkerboard.exe` は完走し市松模様を正しく描画する状態に到達**。
GIMP 3.2.4 の `checkerboard.exe` プラグインを単体で起動し、
GP_CONFIG → GP_PROC_RUN → タイル転送（GET 7 / PUT 16）→ GP_PROC_RETURN(status=SUCCESS)
までのフルサイクルが動作する。

### 完走時の出力（200×200, check-size=16, psychobilly=0）

```
[stats] TileGET=7 TilePUT=16
[dump] build\checkerboard_result.ppm written (200 x 200 RGB)
[dump] px(0,0)=(255,255,255,255) BG  px(16,0)=(0,0,0,255) FG
       px(0,16)=(0,0,0,255) FG       px(16,16)=(255,255,255,255) BG
[exit] exitCode=0x00000000
```

セル (xp, yp) = (x/16, y/16)、`val = (xp&1) != (yp&1)`、`val=false→BG=白`、`val=true→FG=黒`。
4 サンプル点の値はすべて期待通り（cf. `plug-ins/common/checkerboard.c::checkerboard_func`）。

---

## 解決済み問題

### 1. ~~0xC0000005 ACCESS VIOLATION~~（commit `5f7939b`）

GP_PROC_RUN 送信直後の即死。GP_CONFIG ペイロードの完全化 + GP_PROC_RETURN
ハンドリング追加で解決。

### 2. ~~GIMP_PDB_CALLING_ERROR "image out of range"~~（commit `470331a`）

`gimp_param_image_validate` で image 引数が "out of range" 扱いされていた問題。
**根本原因**: `GIMP_PDB_SUCCESS` の値が誤っていた（`GimpPDBStatusType` enum で
`SUCCESS=3` だが `0` を SUCCESS としていた）。`src/ipc/wire_io.h` で `= 3` に修正、
全 PDB 応答に伝搬。

### 3. ~~0xC0000409 STATUS_STACK_BUFFER_OVERRUN at `gimp-drawable-get-format`~~（commit `25e4a69`）

catch-all (status のみ) では format 文字列を返せず、libgimp が NULL Babl format を
GEGL に渡してスタックオーバーラン。

**修正**: `gimp-drawable-get-format` に明示ハンドラを追加し `WriteStringReturn(name, "R'G'B'A u8")`
を返却。同 commit で `gimp-image-get-color-profile` に GIMP_PDB_EXECUTION_ERROR を
返す処理（libgimp は status≠SUCCESS なら NULL profile 扱いとし、
`babl_format_with_space("R'G'B'A u8", NULL)` でデフォルト sRGB space を使う）も追加。

### 4. ~~stdout 全バッファリングでクラッシュ直前のログがフラッシュされない~~

`cmd /c '... > log 2>&1'` でファイルリダイレクトすると stdio が全ブロック
バッファリング (4096B) になり、プラグインクラッシュ時の最終 printf が
ファイルに書かれない問題。

**修正**: `tools/test_checkerboard.cpp::main` 冒頭で
`std::setvbuf(stdout, nullptr, _IONBF, 0)` を呼び stdout / stderr を
アンバッファ化。

### 5. ~~PUT パスのプロンプトでハング~~（決定的バグ）

`HandleTileRequest` の PUT 経路で、プロンプト送信時に `bpp=4, w=64, h=64,
use_shm=0` を送っていた。受信側 `_gp_tile_data_read` (`libgimpbase/gimpprotocol.c:850`)
は `use_shm==0` の時 `width*height*bpp` バイトの pixel_data を必ず読むため、
プラグインが 16384 バイトの読み込みでブロックしていた。

GIMP 本体ホスト (`app/plug-in/gimpplugin-message.c::gimp_plug_in_handle_tile_put`,
line 199-208) は **`bpp=0, w=0, h=0`** の全ゼロプロンプトを送り、
length=0 で pixel_data 不要にしている。

**修正**: `src/host/tile_transfer.cpp` の PUT プロンプトを全ゼロに変更。
副次的に PUT REQ の `tile_num` は常に 0 と判明し（`gimptilebackendplugin.c:413`）、
書き戻し時の `CopyTileToBuffer` 引数を `recvTileNum`（実タイル番号）に修正。

---

## 診断ツール: test_checkerboard.exe

### ビルド

```powershell
& cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && meson compile -C build test_checkerboard'
```

PowerShell から meson を直接呼ぶと cl.exe が見つからない（`vcvars64.bat`
が現在のシェルにアクティブされないため）。

### 実行

プロジェクトルートから実行する（`build/` を cwd にしないこと）。

```powershell
cmd /c '.\build\test_checkerboard.exe > build\checkerboard_test_run.log 2>&1'
```

### Windows プラグインの stderr ログが空になる罠

Windows 版 GIMP プラグインは `gimp_main` 内で
`AttachConsole(ATTACH_PARENT_PROCESS)` 成功 + `TERM`/`SHELL` 未定義の条件で
`freopen("CONOUT$", "r+", stderr)` を呼び stderr を親コンソールに付け替える。
PowerShell から起動した場合この条件が成立し、`SpawnPlugin` で渡した
`build\checkerboard_run_stderr.log` 行きのファイルハンドルがバイパスされる。

回避策: `$env:TERM = 'dumb'` を設定してから起動するとリダイレクトが維持される。
ただし checkerboard は通常運用で stderr に何も書かないため診断目的では効果が薄い。

### 動作

1. **Query フェーズ**: `-query` モードで起動し GP_PROC_INSTALL からパラメーター定義を表示。
2. **Run フェーズ**: 通常モードで起動、GP_CONFIG → GP_PROC_RUN → タイル転送 → GP_PROC_RETURN。
3. **PPM ダンプ**: 結果を `build\checkerboard_result.ppm` に書き出し、
   サンプルピクセル 4 点（(0,0)/(16,0)/(0,16)/(16,16)）を BG/FG 期待値とともに表示。

---

## Wire Protocol — 確定済みの正しい形式

### checkerboard.exe の GP_PROC_RUN パラメーター

| idx | name | wire type | type_name | 値 |
|-----|------|-----------|-----------|-----|
| 0 | run-mode | Int | `GimpRunMode` | `1` (NONINTERACTIVE) |
| 1 | image | Int | `GimpImage` | `IMAGE_ID` (= 1) |
| 2 | drawables | IdArray | outer=`"GimpCoreObjectArray"`, inner=`"GimpItem"` | `[DRAWABLE_ID]` (= 2) |
| 3 | psychobilly | Int | `gboolean` | `0` |
| 4 | check-size | Int | `gint` | `16` |

**確定済み正しい点:**

- `check-type` パラメーターは GIMP 3 では存在しない（GIMP 2 時代の情報は誤り）
- `check-size-unit` は aux 引数のため Wire には乗らない
- IdArray は文字列を 2 本書く: `"GimpCoreObjectArray"` (外側) + `"GimpItem"` (要素)
- `IMAGE_ID` と `DRAWABLE_ID` を別値にする必要あり（プロキシキャッシュ衝突回避）

### GIMP_PDB_* enum 値（GimpPDBStatusType）

GIMP 3.2.4 `libgimpbase/gimpbaseenums.h`:

```
GIMP_PDB_EXECUTION_ERROR = 0
GIMP_PDB_CALLING_ERROR   = 1
GIMP_PDB_PASS_THROUGH    = 2
GIMP_PDB_SUCCESS         = 3   ← 注意: 0 ではない
GIMP_PDB_CANCEL          = 4
```

### GP_TILE_REQ → GP_TILE_DATA フロー

**GET (plugin reads pixels from host)** — `gimptilebackendplugin.c::gimp_tile_get`:

```
plugin → host: GP_TILE_REQ  { drawable_id, tile_num, shadow }
host   → plugin: GP_TILE_DATA { drawable_id, tile_num, shadow,
                                 bpp, width, height, use_shm=0,
                                 pixel_data[w*h*bpp] }
plugin → host: GP_TILE_ACK
```

プラグイン側は `drawable_id/tile_num/shadow/width/height/bpp` を厳密検証
（`gimptilebackendplugin.c:357-362`）。`bpp` は `babl_format_get_bytes_per_pixel(format)`
で 4（RGBA u8）。`width`/`height` は GEGL の `tile->ewidth`/`eheight`。

**PUT (plugin writes pixels to host)** — `gimptilebackendplugin.c::gimp_tile_put`:

```
plugin → host: GP_TILE_REQ  { drawable_id=-1, tile_num=0, shadow=0 }
                              ↑ 全フィールドが固定値（実タイル情報は次の DATA に乗る）
host   → plugin: GP_TILE_DATA { -1, 0, 0, 0, 0, 0, use_shm=0 }
                              ↑ **すべて 0** にすること（length=0 で pixel_data 不要）
plugin → host: GP_TILE_DATA { drawable_id, tile_num, shadow, bpp, w, h,
                              use_shm=0, pixel_data[w*h*bpp] }
host   → plugin: GP_TILE_ACK
```

GIMP 本体 `app/plug-in/gimpplugin-message.c::gimp_plug_in_handle_tile_put` 参照。

---

## 実装済み PDB スタブ（`src/host/pdb_stubs.cpp`）

明示ハンドラ:

- `gimp-image-list` / `gimp-display-list` (IdArray)
- `gimp-image-get-active-drawable` / `gimp-drawable-get` (Int=DRAWABLE_ID)
- `gimp-drawable-get-width` / `-get-height` (Int=m_width/height)
- `gimp-drawable-type` / `-get-image-type` (Int=IMAGE_TYPE_RGBA)
- `gimp-drawable-has-alpha` (gboolean=1)
- `gimp-drawable-get-bpp` (Int=4)
- `gimp-drawable-mask-intersect` (status + 5 ints: non_empty=1, x=0, y=0, w, h)
- `gimp-context-get-foreground` (GeglColor 黒) / `-get-background` (GeglColor 白)
- `gimp-image-id-is-valid` / `gimp-drawable-id-is-valid` / `gimp-item-id-is-valid` (gboolean=1)
- `gimp-item-id-is-layer` / `-is-drawable` (gboolean=1)
- `gimp-item-id-is-text-layer` / `-vector-layer` / `-link-layer` / `-group-layer` /
  `-layer-mask` / `-channel` / `-selection` / `-path` / `-vectors` (gboolean=0)
- `gimp-drawable-is-rgb` (gboolean=1) / `-is-gray` / `-is-indexed` (gboolean=0)
- `gimp-drawable-get-format` (String "R'G'B'A u8")
- `gimp-item-get-image` (Int=IMAGE_ID)
- `gimp-image-get-color-profile` / `-get-effective-color-profile` (status=EXECUTION_ERROR
  → libgimp は NULL profile としてデフォルト sRGB space を使う)

catch-all（status=SUCCESS のみ）: 上記以外。
checkerboard が呼ぶ範囲では `gimp-progress-init` / `gimp-progress-update` /
`gimp-drawable-merge-shadow` / `gimp-drawable-update` がこの経路で問題なく完走。

---

## 次にやること

最終更新: 2026-05-04（コミット `5bd7028` 以降）

### ✅ 完了済み

- Phase-1 standalone E2E（`test_checkerboard.exe`）完走
- Phase-2 CSP 経由 E2E 動作（commit `a713660`）
- 静的プラグイン層アーキテクチャ導入（commit `27327dd`）
- **プラグイン層インターフェース完成（CSPBridge アライン版・commit `5bd7028`）**
  - `plugin_iface.h/cpp` 4 関数 + `PluginInfo` メタデータ + `CreateAsciiString` ヘルパー
  - `checkerboard.cpp` で `psychobilly` を Boolean UI 化、`SetupProperty` / `BuildFilterParams`
    / `OnPropertyChanged` を SDK 直接呼び出しスタイルで実装
  - `plugin_entry.cpp` のメタデータ・プロパティ生成・コールバック処理を全面プラグイン委譲化
  - 既存テスト負債 2 件修正（`test_tile_transfer` PUT プロンプト全ゼロ、`test_wire_io` swap_path 実 path）

### 🚩 次セッション最優先: プレビュー問題の解決

`checkerboard` は `canPreview = false` のためスルーされているが、**`canPreview = true` 系プラグイン
（gauss-blur, hsv 等の典型的フィルター）では現状の実装でプレビューが正しく動かない**懸念がある。
PL レビュー（コミット `5bd7028`）で識別済みの課題:

1. **`OnPropertyChanged` で `Modify` を返さないとプレビュー再描画がトリガーされない**
   - CSPBridge `Blur.cs:157-159` / `HSV.cs:216-218` は `notify == NotifyValueChanged` のとき
     `kTriglavPlugInPropertyCallBackResultModify` を返すパターン
   - 本実装の `checkerboard.cpp::OnPropertyChanged` は常に `NoModify`
   - Doxygen にこの注意は追記済み（`plugin_iface.h::OnPropertyChanged`）が、
     プレビュー実装そのものは未検証

2. **CSP の `FilterRunProcess` ステートマシン未対応**
   - CSPBridge `EffectHelper.RunPreviewLoop` は `Start / Continue / End / Restart / Exit` の
     ステート管理を行いつつブロック単位で処理
   - 本実装の `plugin_entry.cpp::FilterRun` は `Start` を 1 回呼んだ後すぐに GIMP プロセスを
     起動して全タイル転送 → `End` で終了する単発実行のみ
   - プレビュー対応にはこのループを再設計し、Restart で `BuildFilterParams` を再呼出して
     差分プレビューを生成する仕組みが必要

3. **GIMP プロセス再起動コストの問題**
   - プレビューでパラメーター変更のたびに GIMP プラグイン EXE を起動し直すと UX が悪い
   - `PluginSession` 再利用の可否（`RunFilter()` は 1 セッション 1 回限り仕様）を見直すか、
     プレビュー専用の軽量パスを設けるか要検討
   - `wire_io.h::PluginSession` のドキュメントコメント: "RunFilter() は 1 セッションにつき
     1 回のみ呼び出し可（内部 std::promise が 1 回限り）" — この前提を変える必要あり

### 📋 次セッションのアプローチ（推奨）

1. **CSPBridge `EffectHelper.RunPreviewLoop` を C++ で参考再構築**
   - `E:/Projects/CSPBridge/CSPBridgeEffects/Effects/EffectHelper.cs:118` 以降を読む
   - ステート管理を `plugin_entry.cpp::FilterRun` 内に移植
2. **`PluginSession` 再利用設計の検討**
   - 同一 GIMP プロセスに対して複数 `RunFilter()` を投げられるよう `std::promise` 仕様を変更
   - またはプレビュー専用 fast-path（fewer GIMP roundtrips）
3. **検証プラグイン**: 元々の C-1 候補だった `pixelize` か `hsv` を `canPreview = true` で実装し、
   CSP UI でリアルタイムスライダー操作 → プレビュー反映 を確認

### その他の積み残し（プレビュー後）

- **C-1 (pixelize.cpp)**: 静的プラグイン層 2 本目の実証（int パラメーター 1 個のシンプル系）
- **C-2 (gauss_blur.cpp)**: Decimal × 2 + Enumeration の検証。`propSvc2` 経由の Enumeration 実装を実機確認
- **D (spec stale TODO 整理)**: `buffer.cpp:374` / `buffer.h:78,103` の `updateDestinationOffscreenRectProc` TODO 撤回（`plugin_entry.cpp:494` で既に呼ばれている）
- **agent ファイル更新**: `.claude/agents/project-leader.md` に「variant + visit 案不採用 / SDK 直接呼び出し採用」の決裁前例を 1 行追記

### 📂 別 PC で続行する手順

```powershell
git clone <repo>
cd CSPBridgeGimp
git pull
# config/bridge_config.json の plugin_search_paths / gimp_lib_dir / csp_plugin_output_dir を環境に合わせて編集
meson setup build
& cmd /c '"<vcvars64.bat path>" >nul && meson compile -C build'

# 標準 E2E（GIMP 3.2 が PATH に解決可能なら）
.\build\test_checkerboard.exe

# 次セッションの起点: docs/e2e_debug.md の本セクションを読む
# プレビュー実装の参考: E:/Projects/CSPBridge/CSPBridgeEffects/Effects/EffectHelper.cs::RunPreviewLoop
```

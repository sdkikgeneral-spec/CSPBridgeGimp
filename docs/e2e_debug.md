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

Phase-1 の standalone E2E は完了。次フェーズは:

1. CSP プラグイン本体（`MyGimpHost.dll`）への組込み
2. CSP の画像バッファ → HostContext の RGBA バッファへの双方向同期
3. CSP 上で実プラグインを呼び出すサンプル UI

Phase-1 で得た知見は Wire Protocol / PDB スタブ / タイル転送のすべてに反映済みで、
そのまま CSP 統合に流用できる。

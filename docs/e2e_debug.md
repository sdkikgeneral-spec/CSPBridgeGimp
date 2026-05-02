# E2E デバッグノート

最終更新: 2026-05-03

## 現在の状況

フェーズ1の実装（ステップ 5〜11）はすべてコミット済み。
`test_checkerboard.exe` での E2E 実機検証フェーズ。

### 症状（最新 2026-05-03）

`gimp_param_image_validate` の "out of range" は解決済み。
checkerboard は validation を通過し実行段階に入るが、
PDB スタブが不足している地点でプラグインが
`0xC0000409` (STATUS_STACK_BUFFER_OVERRUN, /GS guard) でクラッシュする。

```
... (validation 段で is-text-layer ... is-image-id-is-valid) ...
gimp-drawable-is-rgb     → 1 (TRUE) [明示]
gimp-drawable-is-gray    → 0 (FALSE)[明示]
gimp-drawable-has-alpha  → 1 (TRUE) [明示]
gimp-context-get-foreground → GeglColor
gimp-drawable-mask-intersect → (1, 0, 0, 64, 64) [明示]
gimp-progress-init        → status [catch-all] ※ String + Int 引数
gimp-item-id-is-valid     → 1 [明示]
gimp-drawable-get-format  → status のみ [catch-all] ★未実装
gimp-drawable-get-width   → 64 [明示]
gimp-drawable-get-height  → 64 [明示]
                           ↓
[loop] plugin died (exitCode=0xC0000409)
```

`gimp-drawable-get-format` は GeglFormat (BablFormat) を返すべきだが
catch-all で status のみを返しているため、プラグインが NULL format を
受け取って GEGL バッファ生成時にスタックオーバーランしている可能性が高い。

---

## 解決済み問題

### ~~0xC0000005 ACCESS VIOLATION~~（commit `5f7939b`）

GP_PROC_RUN 送信直後に即死していた問題。
GP_CONFIG ペイロードの完全化 + GP_PROC_RETURN ハンドリング追加で解決。

### ~~GIMP_PDB_CALLING_ERROR "image out of range"~~（commit 本ノート時点で fix 中）

`gimp_param_image_validate` で image 引数が常に "out of range" 扱いされていた問題。

**根本原因**: `GIMP_PDB_SUCCESS` の値が誤っていた（GIMP の `GimpPDBStatusType`
enum 順は EXECUTION_ERROR=0, CALLING_ERROR=1, PASS_THROUGH=2, **SUCCESS=3**, CANCEL=4
だが、コード上は `0` を SUCCESS としていた）。

このため `gimp-image-id-is-valid` の応答 `[status=0, gboolean=1]` を受けた
プラグイン側で `GIMP_VALUES_GET_ENUM(return_vals, 0) == GIMP_PDB_SUCCESS` が
`0 == 3` で常に FALSE、`valid = GIMP_VALUES_GET_BOOLEAN(...)` が実行されず
`valid` は初期値 FALSE のまま、`gimp_image_is_valid` が FALSE を返し
validate のbranch B が image を unref + NULL 化して "out of range" エラー。

`item-id-is-X` は `_gimp_plug_in_get_item` が複数の type 判定を順番に呼ぶ中で
TRUE が返るまで進む構造だったため、status==SUCCESS チェックが効かず
個別の bool 値だけを `gimp_value_array_index(rv,1)` 経由で読んでも結果として
"is-layer" だけ TRUE になり通っていたが、`gimp_image_id_is_valid` のように
status を厳密にチェックする系では機能していなかった（同じ catch-all バグだったが、
`item-id-is-X` 側の挙動でマスクされていた）。

**修正**: `src/ipc/wire_io.h` の `GIMP_PDB_*` 定数を実際の enum 値に合わせて整理。
`GIMP_PDB_SUCCESS = 3`。`WriteProcReturn` のデフォルト引数 / 各 `WriteIntReturn`
内の status param は同定数を使うため、フィックスは 1 行の値変更で全 PDB 応答に伝搬する。

**確認**: テスト再実行で validation 通過 → 実行段階に進入。

---

## 診断ツール: test_checkerboard.exe

### ビルド

```powershell
# vcvars64 を起動した cmd 経由で meson を呼ぶ必要がある
& cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && meson compile -C build test_checkerboard'
```

PowerShell から meson を直接呼ぶと cl.exe が見つからない。
これは `vcvars64.bat` が現在のシェルに自動アクティブされないため。

### 実行

プロジェクトルートから実行する（`build/` を cwd にしないこと）。

```powershell
.\build\test_checkerboard.exe
```

### Windows プラグインの stderr ログが空になる罠

Windows 版 GIMP プラグインは `gimp_main` 内で
`AttachConsole(ATTACH_PARENT_PROCESS)` 成功 + `TERM`/`SHELL` 未定義の条件で
`freopen("CONOUT$", "r+", stderr)` を呼び stderr を親コンソールに付け替える。

PowerShell から起動した場合この条件が成立し、`SpawnPlugin` で渡した
`build\checkerboard_run_stderr.log` 行きのファイルハンドルがバイパスされる。
プラグイン側 `g_warning` / `g_critical` 等の出力は親コンソールに出るが、
`test_checkerboard.exe` は stderr を捕捉していないため画面に流れる。

回避策: `$env:TERM = 'dumb'` を設定してから起動するとリダイレクトが
維持される（が、checkerboard は通常運用で stderr に何も書かないため
診断目的では効果が薄い）。

### 動作

1. **Query フェーズ**: checkerboard.exe を `-query` モードで起動し、
   GP_PROC_INSTALL を受け取ってパラメーター定義を表示する。
2. **Run フェーズ**: checkerboard.exe を通常モードで起動し、
   GP_CONFIG → GP_PROC_RUN を送ってタイル転送の動作を確認する。

stderr ログは `build\checkerboard_run_stderr.log` に保存される（上記の罠あり）。

### 前提

- `%LOCALAPPDATA%/Programs/GIMP 3/lib/gimp/3.0/plug-ins/checkerboard/checkerboard.exe`
  に GIMP 3.2.4 (Windows x64) がインストール済みであること
- `config/bridge_config.json` の `plugin_search_paths` で発見できる場所であること
- `test_checkerboard.exe` は `build/` にコピーする必要なし（search path 経由で発見）

---

## Wire Protocol — 確定済みの正しい形式

checkerboard.exe の GP_PROC_RUN パラメーター（GIMP 3.2.4 ソース確認済み）:

| idx | name | wire type | type_name | 値 |
|-----|------|-----------|-----------|-----|
| 0 | run-mode | Int | `GimpRunMode` | `1` (NONINTERACTIVE) |
| 1 | image | Int | `GimpImage` | `IMAGE_ID` (= 1) |
| 2 | drawables | IdArray | outer=`"GimpCoreObjectArray"`, inner=`"GimpItem"` | `[DRAWABLE_ID]` (= 2) |
| 3 | psychobilly | Int | `gboolean` | `0` |
| 4 | check-size | Int | `gint` | `16` |

**確定済み正しい点:**
- `check-type` パラメーターは GIMP 3 では**存在しない**（GIMP 2 時代の情報は誤り）
- `check-size-unit` は aux 引数のため Wire には乗らない
- IdArray は文字列を **2 本**書く:
  1. `params[i].type_name` = `"GimpCoreObjectArray"` （外側、`_gp_params_read` 外ループで読む）
  2. `d_id_array.type_name` = `"GimpItem"` （内側、IdArray ハンドラーで読む）
- `GpParamType` enum 値は GIMP 3.0.0 と 3.2.4 で**同一**（Curve=15 が 3.2 で追加されたのみ）
- `IMAGE_ID` と `DRAWABLE_ID` を別値にする必要あり（プロキシキャッシュ衝突回避）

### GIMP_PDB_* enum 値（GimpPDBStatusType）

GIMP 3.2.4 `libgimpbase/gimpbaseenums.h` 確認済み:

```
GIMP_PDB_EXECUTION_ERROR = 0
GIMP_PDB_CALLING_ERROR   = 1
GIMP_PDB_PASS_THROUGH    = 2
GIMP_PDB_SUCCESS         = 3   ← 注意: 0 ではない
GIMP_PDB_CANCEL          = 4
```

GP_PROC_RETURN の status param は wire 上 `GP_PARAM_TYPE_INT` で送るが、
受信側で `g_value_init(GIMP_TYPE_PDB_STATUS_TYPE)` → enum として解釈される。
`GIMP_VALUES_GET_ENUM(return_vals, 0) == GIMP_PDB_SUCCESS` 比較は数値ベース。

---

## 次にやること（優先順）

### 1. `gimp-drawable-get-format` の BablFormat 応答実装

現在 catch-all で status のみを返しているため、プラグインが NULL format を
GEGL に渡してスタックオーバーランしていると推定。

GIMP 3.2.4 の wire format (`gimpprotocol.c::_gp_params_write` の
`GP_PARAM_TYPE_BABL_FORMAT` ケース):

```
param_type = INT(6) // GP_PARAM_TYPE_BABL_FORMAT
type_name  = string ("Babl")  // 確認必要
data:
  encoding         = string ("R'G'B'A u8")
  profile_size     = int32 (0 = no ICC)
  profile_data     = bytes[profile_size]
```

`WriteGeglColorReturn` がすでに似た構造を持つので参考になる。

### 2. `gimp-progress-*` のスタブ整理

`gimp-progress-init` (String + Int 引数) は status のみ返せばよさそう。
`gimp-progress-update` / `gimp-progress-end` も catch-all で OK の見込み。

### 3. 残る PDB 呼び出しの洗い出し

`get-format` 修正後にプラグインがどこまで進むかを観察し、
追加で必要なスタブを順次実装する。タイル転送 (`GP_TILE_REQ`) まで
たどり着けば、`tile_transfer.cpp` の HandleTileRequest が機能する。

---

## 調査済み GIMP ソース（GIMP_3_2_4 タグ）

| ファイル | 確認内容 |
|---------|---------|
| `plug-ins/common/checkerboard.c` (3.0.0) | パラメーター定義（psychobilly + check-size + check-size-unit aux） |
| `libgimpbase/gimpprotocol.c` | IdArray 2 文字列フォーマット / `_gp_params_read` `_gp_params_write` 形式 |
| `libgimpbase/gimpprotocol.h` | GP メッセージ enum / `GpParamType` enum 値（3.0/3.2 で互換、Curve=15 のみ追加） |
| `libgimpbase/gimpbaseenums.h` | **`GimpPDBStatusType` enum 値（SUCCESS=3）** |
| `libgimp/gimpgpparams-body.c` | `gimp_gp_param_to_value` `_gimp_gp_params_to_value_array` |
| `libgimp/gimpimageprocedure.c` | run flow — image/drawables 抽出 |
| `libgimp/gimpprocedureconfig.c` | `_gimp_procedure_config_begin_run` — `if (image)` で NULL を防御 |
| `libgimp/gimpimage.c` | `gimp_image_get_by_id` `gimp_image_is_valid` PDB 経由 |
| `libgimp/gimpimage_pdb.c` | `gimp_image_id_is_valid` PDB 呼び出し / status==SUCCESS チェックあり |
| `libgimp/gimpparamspecs-body.c` | `gimp_param_image_validate` — branch A: image NULL, branch B: invalid via `gimp_image_is_valid` |
| `libgimp/gimpitem.c` | `_gimp_plug_in_get_item` — type 判定で複数 PDB を順次呼ぶ |
| `libgimp/gimpplugin.c` | `_gimp_plug_in_get_image/_item`, `gimp_plug_in_proc_run_internal` |
| `libgimp/gimppdb.c` | `_gimp_pdb_run_procedure_array` — return をそのまま GValueArray 化、pspec=NULL |
| `libgimp/gimp.c` | Windows: `freopen("CONOUT$", stderr)` 条件 |
| `libgimp/gimp-debug.c` | `GIMP_PLUGIN_DEBUG` env var キー一覧 |

---

## `src/host/pdb_stubs.cpp` の現在実装済みスタブ

明示ハンドラ（n_params=2、status + 値）:
- `gimp-image-list` / `gimp-display-list` (IdArray)
- `gimp-image-get-active-drawable` / `gimp-drawable-get` (Int=DRAWABLE_ID)
- `gimp-drawable-get-width` / `-get-height` (Int=m_width/height)
- `gimp-drawable-type` / `-get-image-type` (Int=IMAGE_TYPE_RGBA)
- `gimp-drawable-has-alpha` (gboolean=1)
- `gimp-drawable-get-bpp` (Int=4)
- `gimp-drawable-mask-intersect` (status + 5 ints)
- `gimp-context-get-foreground` (GeglColor 黒) / `-get-background` (GeglColor 白)
- `gimp-image-id-is-valid` / `gimp-drawable-id-is-valid` / `gimp-item-id-is-valid` (gboolean=1)
- `gimp-item-id-is-layer` / `-is-drawable` (gboolean=1)
- `gimp-item-id-is-text-layer` / `-vector-layer` / `-link-layer` / `-group-layer` /
  `-layer-mask` / `-channel` / `-selection` / `-path` / `-vectors` (gboolean=0)
- `gimp-drawable-is-rgb` (gboolean=1) / `-is-gray` / `-is-indexed` (gboolean=0)

catch-all（status のみ）— 該当 PDB が呼ばれた場合 wire 上 n_params=1, status=SUCCESS:
- 上記以外すべて
- 既知で要実装: `gimp-drawable-get-format`（BablFormat 必須）

定数（`pdb_stubs.h`）:
```cpp
static constexpr int32_t IMAGE_ID        = 1;
static constexpr int32_t DRAWABLE_ID     = 2;
static constexpr int32_t IMAGE_TYPE_RGBA = 1;
```

`GIMP_PDB_SUCCESS` 等のステータス定数は `wire_io.h` で定義（`= 3`）。

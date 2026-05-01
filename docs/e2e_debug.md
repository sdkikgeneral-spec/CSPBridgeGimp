# E2E デバッグノート

最終更新: 2026-05-01

## 現在の状況

フェーズ1の実装（ステップ 5〜11）はすべてコミット済み。
現在は **checkerboard.exe を実際に動かす E2E 検証**を行っている。

### 症状

`test_checkerboard.exe` で GP_PROC_RUN を送ると、プラグインが即死する。

```
[loop] Entering message loop immediately after GP_PROC_RUN...
  [loop] no data [1/20]
  [loop] plugin died
[stats] TileGET=0 TilePUT=0
[exit] exitCode=0xC0000005
[stderr] Plugin stderr log: (empty)
```

- **終了コード**: `0xC0000005` = Windows ACCESS VIOLATION（NULL ポインターデリファレンス）
- **PDB コール**: 1 件も届かない（タイルリクエストもなし）
- **stderr**: 空（GLib の `g_warning` / `g_critical` も出ない）

---

## 診断ツール: test_checkerboard.exe

### ビルド

```powershell
meson setup --reconfigure build   # 初回または meson.build 変更後
meson compile -C build test_checkerboard
```

### 実行

プロジェクトルートから実行する（`build/` を cwd にしないこと）。

```powershell
.\build\test_checkerboard.exe
```

### 動作

1. **Query フェーズ**: checkerboard.exe を `-query` モードで起動し、
   GP_PROC_INSTALL を受け取ってパラメーター定義を表示する。
2. **Run フェーズ**: checkerboard.exe を通常モードで起動し、
   GP_CONFIG → GP_PROC_RUN を送ってタイル転送とクラッシュを観察する。

stderr ログは `build\checkerboard_run_stderr.log` に保存される。

### 前提

- `build\checkerboard.exe` が存在すること（GIMP インストールフォルダーからコピーまたはシンボリックリンク）
- `build\` に `gimp-3.0.dll`, `libglib-2.0-0.dll` など GIMP 依存 DLL が揃っていること
- GIMP 3.2.4（Windows x64）でビルドされた checkerboard.exe を想定

標準的な GIMP のインストールパス:
```
C:\Program Files\GIMP 3\lib\gimp\3.0\plug-ins\checkerboard\checkerboard.exe
C:\Program Files\GIMP 3\bin\  ← DLL 群
```

---

## Wire Protocol — 確定済みの正しい形式

checkerboard.exe の GP_PROC_RUN パラメーター（GIMP 3.0.0 ソース確認済み）:

| idx | name | wire type | type_name | 値 |
|-----|------|-----------|-----------|-----|
| 0 | run-mode | Int | `GimpRunMode` | `1` (NONINTERACTIVE) |
| 1 | image | Int | `GimpImage` | `IMAGE_ID` (= 1) |
| 2 | drawables | IdArray | outer=`"GimpCoreObjectArray"`, inner=`"GimpItem"` | `[DRAWABLE_ID]` (= 2) |
| 3 | psychobilly | Int | `gboolean` | `0` |
| 4 | check-size | Int | `gint` | `16` |

**注意点:**
- `check-type` パラメーターは GIMP 3 では**存在しない**（GIMP 2 時代の情報は誤り）
- `check-size-unit` は aux 引数のため Wire には乗らない
- IdArray は文字列を **2 本**書く:
  1. `params[i].type_name` = `"GimpCoreObjectArray"` （GValue の GType 名）
  2. `d_id_array.type_name` = `"GimpItem"` （要素 GType 名）

---

## 根本原因の仮説

### 調査で判明した libgimp の実行フロー

```
checkerboard.exe が GP_PROC_RUN を受信
  └─ _gp_params_read()          // Wire を GpParam[] に変換
  └─ _gimp_gp_params_to_value_array()
       └─ gimp_gp_param_to_value() × 5
            param[1]: type_name="GimpImage", data.d_int=1
              └─ g_type_from_name("GimpImage") → GType
              └─ gimp_image_get_by_id(1)
                   └─ _gimp_plug_in_get_image(plug_in, 1)
                        → ローカルキャッシュを検索
                        → 未登録 → NULL を返す ← ★ここが疑惑点
  └─ gimp_image_procedure_run()
       image = GIMP_VALUES_GET_IMAGE(args, 1)  // NULL
       _gimp_procedure_config_begin_run(config, NULL, ...)  // if(image) で防御済み
       run_func(procedure, run_mode, NULL, drawables, ...)  // NULL を渡す
  └─ checkerboard.c run_func
       // image == NULL を前提としていない → ACCESS VIOLATION ← ★クラッシュ箇所
```

### 仮説の根拠

- `_gimp_procedure_config_begin_run` は `if (image)` で NULL を防御している（ソース確認済み）
- クラッシュ前に PDB コールが 1 件も届かない
  → `_gimp_plug_in_get_image` が PDB を呼ばずキャッシュのみを見ている
  → 結果 NULL が返り、その NULL が `run_func` まで伝播してクラッシュ

### 未確認事項

- `_gimp_plug_in_get_image` の実装（GIMP ソース要確認）
  - キャッシュヒットなしのとき NULL を返すのか、それともプロキシを生成するのか
  - もしプロキシを生成するなら PDB コールが来るはずだが来ていない

---

## 次にやること

### 調査 A: `_gimp_plug_in_get_image` の実装を確認する

```
https://gitlab.gnome.org/GNOME/gimp/-/raw/GIMP_3_0_0/libgimp/gimpplugin.c
```

キャッシュのみか、プロキシ生成か確認する。

### 調査 B: 切り分け実験

`test_checkerboard.cpp` の `WriteCheckerboardRun()` で image ID を変えて実験する。

```cpp
// image_id = -1 で送ってみる（libgimp が早期 NULL チェックするか確認）
ch.WriteInt32(-1);  // IMAGE_ID の代わり
```

- `-1` でも同じクラッシュ → `_gp_params_read` 以降が問題
- `-1` で挙動が変わる → IMAGE_ID の値依存の問題

### 調査 C: checkerboard.c の run_func を読む

run_func が image に対して何をするか確認する。

```
https://gitlab.gnome.org/GNOME/gimp/-/raw/GIMP_3_0_0/plug-ins/common/checkerboard.c
```

`gimp_drawable_get_buffer()` を呼ぶ前に NULL チェックがあるか確認。

---

## 調査済み GIMP ソース（GIMP_3_0_0 タグ）

| ファイル | 確認内容 |
|---------|---------|
| `plug-ins/common/checkerboard.c` | パラメーター定義（psychobilly + check-size + check-size-unit） |
| `libgimpbase/gimpprotocol.c` | IdArray の 2 文字列フォーマット確認 |
| `libgimp/gimpgpparams-body.c` | `gimp_gp_param_to_value` — Int 型で `g_type_from_name` + `gimp_image_get_by_id` 呼び出しを確認 |
| `libgimp/gimpimageprocedure.c` | run flow — `image`/`drawables` に NULL チェックなし |
| `libgimp/gimpprocedureconfig.c` | `_gimp_procedure_config_begin_run` — `if (image)` で NULL を防御 |
| `libgimp/gimpimage.c` | `gimp_image_get_by_id` → `_gimp_plug_in_get_image` を呼ぶことを確認 |
| `libgimp/gimpitem.c` | `gimp_item_get_by_id` → `_gimp_plug_in_get_item` → PDB コール（参考）|
| `libgimp/gimpplugin.c` | `_gimp_plug_in_get_item` が PDB を呼ぶことを確認（image 版は未確認）|

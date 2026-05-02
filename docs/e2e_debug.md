# E2E デバッグノート

最終更新: 2026-05-02

## 現在の状況

フェーズ1の実装（ステップ 5〜11）はすべてコミット済み。
`test_checkerboard.exe` でのE2E実機検証フェーズ。

### 症状

`test_checkerboard.exe` で GP_PROC_RUN を送ると、
PDB コールは正常に往復するが最終的に **GIMP_PDB_CALLING_ERROR** で失敗する。

```
name='gimp-item-id-is-text-layer' n_params=1  [Int=1]
name='gimp-item-id-is-vector-layer' n_params=1  [Int=1]  ← catch-all (n_params=1)
name='gimp-item-id-is-link-layer' n_params=1  [Int=1]
name='gimp-item-id-is-group-layer' n_params=1  [Int=1]
name='gimp-item-id-is-layer' n_params=1  [Int=1]
name='gimp-item-id-is-layer-mask' n_params=1  [Int=1]
name='gimp-item-id-is-selection' n_params=1  [Int=1]
name='gimp-item-id-is-channel' n_params=1  [Int=1]
name='gimp-item-id-is-path' n_params=1  [Int=1]
name='gimp-image-id-is-valid' n_params=1  [Int=1]
[loop] recv 6 (GP_PROC_RETURN)
    name='plug-in-checkerboard' n_params=2
    [0] Int = 1  (GIMP_PDB_CALLING_ERROR)
    [1] String = 'プロシージャー 'plug-in-checkerboard' 呼び出し時の引数
        'image' (2 番目の引数、GimpImage 型) の値 '<not transformable to string>'
        は、適正値の範囲外です。'
```

- **終了コード**: 0 (正常終了だが PDB エラー)
- **エラー箇所**: `image` 引数の GParamSpec バリデーション失敗
- **エラー内容**: `<not transformable to string>` = GValue の image ポインターが NULL

---

## 解決済み問題

### ~~0xC0000005 ACCESS VIOLATION~~（解決済み、commit `5f7939b`）

以前は GP_PROC_RUN 送信直後に即死していた。
commit `5f7939b`（GP_CONFIG ペイロードの完全化 + GP_PROC_RETURN ハンドリング追加）で解決済み。
現在は PDB コールが正常に往復するまで進んでいる。

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
   GP_CONFIG → GP_PROC_RUN を送ってタイル転送の動作を確認する。

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
| 2 | drawables | IdArray | outer=`"GimpCoreObjectArray"`, inner=`"GimpItem"` | `[DRAWABLE_ID]` (= 1) |
| 3 | psychobilly | Int | `gboolean` | `0` |
| 4 | check-size | Int | `gint` | `16` |

**確定済み正しい点:**
- `check-type` パラメーターは GIMP 3 では**存在しない**（GIMP 2 時代の情報は誤り）
- `check-size-unit` は aux 引数のため Wire には乗らない
- IdArray は文字列を **2 本**書く:
  1. `params[i].type_name` = `"GimpCoreObjectArray"` （外側、`_gp_params_read` 外ループで読む）
  2. `d_id_array.type_name` = `"GimpItem"` （内側、IdArray ハンドラーで読む）
- `GpParamType` enum 値は GIMP 3.0.0 と 3.2.4 で**同一**（Curve=15 が 3.2 で追加されたのみ）

---

## 根本原因の仮説

### 確定した実行フロー（GIMP ソース確認済み）

```
checkerboard.exe が GP_PROC_RUN を受信
  └─ _gp_params_read()               // Wire を GPParam[] に変換
  └─ gimp_gp_params_to_value_array()
       └─ gimp_gp_param_to_value() × 5
            param[1]: type_name="GimpImage", d_int=1
              └─ g_type_from_name("GimpImage") → GType
              └─ _gimp_plug_in_get_image(plug_in, 1)
                   → キャッシュヒットなし → プロキシを生成して返す（NULL にはならない）
            param[2]: IdArray "GimpCoreObjectArray" / "GimpItem" ids=[1]
              └─ _gimp_plug_in_get_item(plug_in, 1)
                   → GType 特定のために PDB を呼ぶ:
                     gimp-item-id-is-text-layer → FALSE
                     gimp-item-id-is-vector-layer → ★未ハンドル（後述）
                     gimp-item-id-is-link-layer → FALSE
                     gimp-item-id-is-group-layer → FALSE
                     gimp-item-id-is-layer → TRUE → GimpLayer として確定
  └─ gimp_procedure_validate_args()  // GParamSpec バリデーション
       image 引数の GParamSpec (GimpParamSpecImage) で:
         gimp_param_image_validate(pspec, &image_value) を呼ぶ
           └─ gimp_image_is_valid(image_proxy)
                └─ gimp_image_id_is_valid(1)   ← PDB コール
                     我々 → TRUE を返す
                → TRUE を受け取る → 検証通過のはず
       → しかし直後に CALLING_ERROR "image out of range"  ← ★原因不明
```

### 確認済みの事実

- `_gimp_plug_in_get_image`: キャッシュミスでも**必ずプロキシを返す**（NULL を返さない）
  → "image=NULL でクラッシュ" 仮説は否定済み
- `gimp_image_id_is_valid`: PDB コールであることを `gimpimage_pdb.c` で確認済み
- `gimp_param_image_validate`: `gimp_image_is_valid(image)` が FALSE の場合のみ
  バリデーション失敗 → image ポインターを NULL にして return TRUE
- エラーメッセージの `<not transformable to string>` は GValue の v_pointer が NULL であることを示す
  → バリデーション時に image が NULL になっている

### 未解決の疑問

我々が `gimp-image-id-is-valid(1)` に対して TRUE を返しているにもかかわらず、
なぜ `gimp_image_is_valid` が FALSE を返すのか？

**候補 1: `gimp-item-id-is-vector-layer` が catch-all で n_params=1 を返している**

`pdb_stubs.cpp` の Dispatch に `gimp-item-id-is-vector-layer` の明示ハンドラーがなく、
catch-all `WriteProcReturn(name)` が n_params=1（status のみ）を返す。
本来は n_params=2（status + gboolean）が必要。

`GIMP_VALUES_GET_BOOLEAN(return_vals, 1)` が要素数1の配列に対して呼ばれると
`gimp_value_array_index` が `g_return_val_if_fail` により NULL を返し、
`g_value_get_boolean(NULL)` が内部的に FALSE を返す（可能性あり）。
→ 結果: is-vector-layer = FALSE（期待通り）のはず。
→ ただし、return_vals に NULL が混入した状態で後続処理がどう動くか不明。

**候補 2: IMAGE_ID と DRAWABLE_ID が同値 (= 1) でのコンフリクト**

`_gimp_plug_in_get_image(plug_in, 1)` も `_gimp_plug_in_get_item(plug_in, 1)` も
同じキャッシュに ID=1 でエントリーしようとする。
GimpImage プロキシと GimpItem プロキシが同一ハッシュキーで衝突する可能性。

**候補 3: GIMP 3.2 での `gimp_image_is_valid` の挙動変化**

テスト対象は GIMP 3.2.4 だが、ソース確認は GIMP_3_0_0 タグで行っている。
3.2 で `gimpimageprocedure.c` や `gimpparamspecs-body.c` が変更されている可能性。

---

## 次にやること（優先順）

### Fix A: `gimp-item-id-is-vector-layer` を明示ハンドル

`src/host/pdb_stubs.cpp` の Dispatch に追加:

```cpp
else if (   name == "gimp-item-id-is-vector-layer"
         || name == "gimp_item_id_is_vector_layer")
{
    WriteIntReturn(channel, name, "gboolean", 0);
}
```

`gimp-item-id-is-group-layer` の近くに追加するのが自然。

### Fix B: IMAGE_ID と DRAWABLE_ID を別値にする

`pdb_stubs.h` の定数:
```cpp
static constexpr int32_t IMAGE_ID    = 1;
static constexpr int32_t DRAWABLE_ID = 2;  // 1 から 2 に変更
```

`WriteCheckerboardRun()` の IdArray も `DRAWABLE_ID` を使っているか確認:
`tools/test_checkerboard.cpp` の `WriteCheckerboardRun` 関数内の `ch.WriteInt32(DRAWABLE_ID)` 相当箇所。

### 調査 C: GIMP 3.2 の `gimpimageprocedure.c` / `gimpparamspecs-body.c` を確認

```
https://gitlab.gnome.org/GNOME/gimp/-/raw/GIMP_3_2_4/libgimp/gimpimageprocedure.c
https://gitlab.gnome.org/GNOME/gimp/-/raw/GIMP_3_2_4/libgimp/gimpparamspecs-body.c
```

GIMP 3.2.4 でバリデーション方法が変わった場合、
`gimp_image_is_valid` が PDB を呼ばずローカルチェックになっている可能性。

---

## 調査済み GIMP ソース

| ファイル | タグ | 確認内容 |
|---------|------|---------|
| `plug-ins/common/checkerboard.c` | 3_0_0 | パラメーター定義（psychobilly + check-size + check-size-unit aux） |
| `libgimpbase/gimpprotocol.c` | 3_0_0 | IdArray の 2 文字列フォーマット確認 |
| `libgimp/gimpgpparams-body.c` | 3_0_0 | `gimp_gp_param_to_value` — G_VALUE_HOLDS_BOOLEAN ブランチ確認 |
| `libgimp/gimpimageprocedure.c` | 3_0_0 | run flow — image/drawables に NULL チェックなし |
| `libgimp/gimpprocedureconfig.c` | 3_0_0 | `_gimp_procedure_config_begin_run` — `if (image)` で NULL を防御 |
| `libgimp/gimpimage.c` | 3_0_0 | `gimp_image_get_by_id` → `_gimp_plug_in_get_image` を呼ぶことを確認 |
| `libgimp/gimpimage.c` | 3_2 | `gimp_image_is_valid` → `gimp_image_id_is_valid(id)` 呼び出し確認 |
| `libgimp/gimpimage_pdb.c` | 3_2 | `gimp_image_id_is_valid` が PDB コールであることを確認 |
| `libgimp/gimpparamspecs-body.c` | 3_2 | `gimp_param_image_validate` — `gimp_image_is_valid` 呼び出し確認 |
| `libgimp/gimpitem.c` | 3_0_0 | `gimp_item_get_by_id` → `_gimp_plug_in_get_item` → PDB コール（GType 特定）|
| `libgimp/gimpplugin.c` | 3_0_0 | `_gimp_plug_in_get_item` が PDB を呼ぶことを確認 |
| `libgimpbase/gimpprotocol.h` | 3_0_0, 3_2 | `GpParamType` enum 値 — 3.0 と 3.2 で同一（Curve=15 のみ追加）|

---

## `src/host/pdb_stubs.cpp` の構造（参考）

`HostContext::Dispatch` に全 PDB スタブが集中。
定数（`pdb_stubs.h`）:
```cpp
static constexpr int32_t IMAGE_ID    = 1;
static constexpr int32_t DRAWABLE_ID = 1;   // ← Fix B で 2 に変更予定
static constexpr int32_t IMAGE_TYPE_RGBA = 1;
static constexpr int32_t GIMP_PDB_SUCCESS = 0;
```

応答ヘルパー:
- `WriteIntReturn` — status + Int 1 値
- `WriteMultiIntReturn` — status + 複数 Int
- `WriteGeglColorReturn` — status + GeglColor (BABL "R'G'B'A u8")
- `WriteIdArrayReturn` — status + IdArray
- `channel.WriteProcReturn(name)` — status のみ（n_params=1）← catch-all

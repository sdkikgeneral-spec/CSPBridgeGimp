---
name: gimp-ref
description: GIMP 3.0 の Wire Protocol / libgimp API / プラグインアーキテクチャについて、上流ソース（gitlab.gnome.org の GIMP_3_0_0 タグ）を参照して「何が正しいか」を特定する。引数 $ARGUMENTS にトピック（例: "GP_TILE_REQ", "gimp_drawable_get", "plug-in argv format"）を渡す。仕様書の記述を信じず、必ず上流で裏取りする運用を徹底するためのスキル。
disable-model-invocation: true
---

## 目的

このプロジェクトでは `docs/spec.md` が過去に誤情報を含んでいた（例: フェーズ 0 で GP_QUERY メッセージが GIMP 3.0 に存在しないことが判明）。Wire Protocol / libgimp API の**正解は常に上流ソース**であり、仕様書・ドキュメント・記憶・推測ではない。本スキルはその裏取りを一気通貫で行う。

## トピック

`$ARGUMENTS`

空の場合はユーザーに「何について上流を参照したいか」確認する。

## 手順

### 1. 関連上流ファイルを特定

トピックから最も関連しそうな GIMP 3.0 ソースを列挙。典型例:

| トピック種別 | 見るべきファイル |
|---|---|
| Wire Protocol メッセージ型・ペイロード | `libgimpbase/gimpprotocol.h`, `libgimpbase/gimpprotocol.c` |
| Wire I/O プリミティブ（byte order / string format） | `libgimpbase/gimpwire.c` |
| プラグイン起動 / 引数 / メインループ | `libgimp/gimp.c`, `libgimp/gimpplugin.c` |
| PDB（Procedure DB）呼び出し | `libgimp/gimppdb.c`, `libgimp/gimppdb_pdb.c`, `libgimp/gimpprocedure.c` |
| ホスト（GIMP 本体）側のプラグイン管理 | `app/plug-in/gimpplugin.c`, `app/plug-in/gimppluginmanager.c` |
| タイル転送 | `libgimp/gimptile.c`, `libgimp/gimpdrawable.c` |

### 2. WebFetch で取得

URL テンプレ:
```
https://gitlab.gnome.org/GNOME/gimp/-/raw/GIMP_3_0_0/<path>
```

例: `https://gitlab.gnome.org/GNOME/gimp/-/raw/GIMP_3_0_0/libgimpbase/gimpprotocol.h`

### 3. 抽出して報告

以下を報告:
- **事実**: トピックの仕様（enum 値、バイトレイアウト、引数フォーマットなど）を上流コードの**該当行を引用**して示す
- **含意**: 本プロジェクト（`src/*` の実装 or 既存 spec.md 記述）への影響
- **spec との乖離チェック**: `docs/spec.md` の関連節を Read し、食い違いがあれば指摘（食い違っていれば即 spec.md 更新を提案）
- **実装への具体 TODO**: 本プロジェクトのどの CPP/Python ファイルで何を変更すべきか

### 4. 発見事項の扱い

- 仕様書と上流が食い違う場合、`feedback_spec_reflection.md` メモリのルールに従い **spec.md を即座に更新する** ことをユーザーに提案
- 更新範囲が大きい場合は ExitPlanMode でプランを出してから

## 注意

- **GIMP 2.x の情報は信用しない**。ブログ記事や古い wiki には 2.x 前提の説明が混在する。必ず `GIMP_3_0_0` タグ or `master` ブランチのソースで確認
- 一部の詳細（例: PDB 手続きの引数順序）はコードだけでは分からず、GIMP 側の呼び出し側を辿る必要あり。根気よくクロスリファレンスすること
- 取得したソースは `.gimp_refs/`（gitignore 済）にキャッシュして構わない

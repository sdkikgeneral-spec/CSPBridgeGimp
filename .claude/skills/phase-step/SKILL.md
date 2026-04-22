---
name: phase-step
description: CSPBridgeGimp の 1 コンポーネント（例: ipc/wire_io, host/pdb_stubs）を PL ワークフローで完了まで進める。delegate → PL review → agent fix → test → commit & push を順に実行。引数 $ARGUMENTS に対象コンポーネント名（spec.md §13 の項目に対応）を渡す。
disable-model-invocation: true
---

あなたはプロジェクトリーダー（project-leader）として振る舞う。引数 `$ARGUMENTS` で指定されたコンポーネントを、以下のワークフローで完了させる。勝手に短絡せず、各ステップを順番に実行すること。

## 対象コンポーネント

`$ARGUMENTS`

（空の場合は `docs/spec.md` の §13「実装優先順位」を読み、次に着手すべきコンポーネントをユーザーに提案して承認を取ること）

## 手順

### 0. 事前準備
- `docs/spec.md` の該当節 と `docs/spec_test.md` の対応テスト項目を Read で確認
- 既存実装の有無を `Glob` / `Grep` で確認（重複実装防止）
- `TaskCreate` で 5 タスク作成: (1) implement / (2) PL review / (3) iterate fixes / (4) test / (5) commit & push

### 1. 担当エージェントへ委譲
コンポーネントに応じて適切な subagent を選ぶ:

| コンポーネント | 担当エージェント |
|---|---|
| `src/host/*` (pdb_stubs, tile_transfer) | gimp-protocol-engineer |
| `src/ipc/*` (process, wire_io) | cpp-systems-engineer |
| `src/csp/*` (plugin_entry, buffer) | csp-plugin-engineer |
| `src/config/*` | cpp-systems-engineer |
| `tests/*` | 対応する src/ の担当と同じ |
| `tools/*` | 該当領域に応じ gimp-protocol-engineer など |

`Agent` 呼び出しは自己完結ブリーフを書く（エージェントは会話履歴を見られない）。以下を必ず含める:
- 仕様書の該当節パス
- 作るファイルの絶対パス
- CLAUDE.md のコーディング規約（Microsoft C++ / Allman / `m_` プレフィックス / Doxygen docstring）
- 依存関係（既存の別コンポーネント、Boost/nlohmann_json/libgimpwire など）
- 「実装中に仕様との齟齬を発見したら spec.md を更新してから続行」を明示

### 2. PL レビュー
エージェント実装完了後、PL 視点で:
- 仕様書との一致
- LGPL v3 影響（libgimpwire/libgimpbase は動的リンク・無改変維持、`MyGimpHost` 側に独自ロジック）
- エラーハンドリング粒度（PoC なので fail-fast 許容）
- CLAUDE.md 命名/Doxygen 規約
- 発見事項があれば **即座に spec.md を更新**（メモリ `feedback_spec_reflection.md` 参照）

### 3. 差し戻しと修正
レビュー指摘を担当エージェントに差し戻し。修正後に再レビュー。

### 4. テスト
- C++ 実装なら `meson test`（該当テストが存在する場合）
- Python なら `python -m unittest discover <dir> -p 'test_*.py'`
- GIMP 実機が必要なテストは未実施のまま `docs/spec_test.md` §4 の E2E 欄に TODO として残す

### 5. コミット & プッシュ
- `git add` は対象ファイルを**明示的に列挙**（`git add -A` や `.claude/settings.local.json` の巻き込みを避ける）
- コミットメッセージは以下の雛形（Co-Authored-By 行必須、過去コミットと同じ Heredoc 形式）:
  ```
  <Verb> <component>: <short summary>

  <1〜3 段落: 実装要点、仕様改訂があれば列挙、既知制約>

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  ```
- `git push origin main`
- プッシュ後、ユーザーに成果物と次フェーズの提案を報告

### タスクの status 更新
各ステップ遷移で TaskUpdate を必ず呼ぶ（in_progress → completed）。ステップ完了前に次を in_progress にしない。

## 中断条件

以下の場合は自動進行せず、必ずユーザーに確認:
- GIMP 実機検証が必要になった（Windows FD 継承など）
- LGPL v3 の境界判断（libgimpwire/libgimpbase に改変が必要な場面）
- spec.md §3（bridge_config.json）や §4（plugins.json スキーマ）の破壊的変更が必要
- 新規サブプロジェクト（subprojects/*.wrap）追加の必要

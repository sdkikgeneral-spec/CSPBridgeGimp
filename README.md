# CSPBridgeGimp

> **本プロジェクトは技術検証（PoC）を目的としたものであり、有料コンテンツ・商用製品ではありません。**

Clip Studio Paint (CSP) プラグインから GIMP フィルタープラグインをブリッジ実行するホストエミュレーター。

---

## 概要

CSP プラグイン（C++）が GIMP ホストを模倣し、GIMP プラグイン EXE を子プロセスとして起動。Wire Protocol（パイプ IPC）でタイル送受信を行い、処理結果を CSP レイヤーに書き戻す。

```
CSP プラグイン（C++）
  ↓ 画像バッファ取得・RGBA 変換
  ↓ GIMP プラグイン EXE を子プロセス起動
  ↕ Wire Protocol でパイプ通信（タイル送受信）
  ↓ 処理結果を CSP レイヤーに書き戻し
```

一度ブリッジを実装すれば、多数の GIMP フィルタープラグインがそのまま動作する汎用基盤となる。

---

## 対応プラットフォーム

| OS | アーキテクチャ | SIMD |
|---|---|---|
| Windows | x86-64 | SSE2 / SSE4.1 / AVX2 |
| Mac | ARM64 (Apple Silicon) | NEON |

---

## 技術スタック

- **言語**: C++23
- **コーディング規約**: Microsoft C++ スタイル（`.clang-format` で管理）
- **ビルド**: Meson
- **依存ライブラリ**:
  - `libgimpwire` / `libgimpbase`（GIMP、LGPL v3、無改変で動的リンク）
  - `nlohmann/json`（MIT、JSON 設定ファイル読み込み）
  - `Boost`（BSL-1.0、Boost.Process によるクロスプラットフォームプロセス起動）
  - `Catch2`（BSL-1.0、テストフレームワーク）

---

## ビルド

```bash
meson setup build
cd build
meson compile
meson test          # テスト実行
meson install       # bridge_config.json の csp_plugin_output_dir にインストール
```

初回セットアップ時に依存ライブラリを取得:

```bash
meson subprojects download
```

---

## 設定

`config/bridge_config.json` でプラグインのインストール先などを設定する。

```json
{
  "windows": {
    "plugin_search_paths": [
      "C:/Program Files/GIMP 3/lib/gimp/3.0/plug-ins",
      "%APPDATA%/GIMP/3.0/plug-ins"
    ],
    "gimp_lib_dir": "C:/Program Files/GIMP 3/bin",
    "csp_plugin_output_dir": "%APPDATA%/CELSYSUserData/CELSYS/CLIPStudioModule/PlugIn/PAINT"
  },
  "mac": {
    "plugin_search_paths": [
      "/Applications/GIMP.app/Contents/Resources/lib/gimp/3.0/plug-ins",
      "{HOME}/Library/Application Support/GIMP/3.0/plug-ins"
    ],
    "gimp_lib_dir": "/Applications/GIMP.app/Contents/Resources/lib",
    "csp_plugin_output_dir": "{HOME}/Library/Application Support/CELSYSUserData/CELSYS/CLIPStudioModule/PlugIn/PAINT"
  }
}
```

`%APPDATA%` / `%USERPROFILE%`（Windows）および `{HOME}`（Mac）はランタイムで自動展開される。

---

## ライセンス

- `libgimpwire` / `libgimpbase`: LGPL v3（動的リンク、無改変）
- `MyGimpHost`（本リポジトリの実装）: 独自ライセンス（非公開）
- `nlohmann/json`: MIT
- `Boost`: BSL-1.0
- `Catch2`: BSL-1.0

---

## ドキュメント

- [技術調査・ライセンス確認](docs/GIMP_Bridge_Summary.md)
- [実装仕様書](docs/spec.md)
- [テスト仕様書](docs/spec_test.md)

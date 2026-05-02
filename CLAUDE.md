# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CSPBridgeGimp is a C++ project that bridges Clip Studio Paint (CSP) plugins to run GIMP plugins. The CSP plugin acts as a GIMP host emulator, launching GIMP plugin executables as child processes and communicating with them via GIMP's Wire Protocol (IPC over pipes).

**このプロジェクトは技術検証（PoC）を目的としており、有料コンテンツ・商用製品ではありません。**

This project is currently in the **E2E verification phase** (フェーズ1実装完了済み、実機デバッグ中). See `docs/GIMP_Bridge_Summary.md` for the technical investigation and architecture details (written in Japanese).

## Architecture

### Core Concept

```
CSP Plugin (C++) → shared memory/temp buffer → spawn GIMP plugin EXE as child process
                ↕ Wire Protocol over pipes (tile transfer)
                → write result back to CSP layer
```

### Key Components (planned)

| Module | Role |
|--------|------|
| `libgimpwire.dll` | Wire Protocol serialization/deserialization — use unmodified from GIMP source |
| `libgimpbase.dll` | Basic types and parameter definitions — use unmodified |
| `MyGimpHost.dll` | Custom host-side stubs for `libgimp` procedures (proprietary) |

### Minimum Host Stubs Required

The host emulator must implement stubs for:
- `gimp_image_list()`
- `gimp_drawable_get()`
- `gimp_tile_get()` / `gimp_tile_put()` — the core tile transfer layer
- `gimp_display_list()`
- UI/dialog calls — stub to no-op (use `GIMP_RUN_NONINTERACTIVE` to minimize these)

### Licensing

All GIMP libraries (`libgimpwire`, `libgimpbase`, `libgimp`) are **LGPL v3**. Use dynamic linking (DLL) to keep the CSP plugin source proprietary. Only modifications to GIMP libraries themselves require disclosure. The GIMP core/app (GPL v3) is never linked directly.

## Language

**Pure C++23 throughout.** No .NET/C# or other runtimes. This distinguishes CSPBridgeGimp from sibling projects (e.g., CSPBridgeSolidFill which uses .NET). Do not introduce non-C++ components.

**コーディング規約: Microsoft C++ スタイル**（`.clang-format` で強制）

| 項目 | 規則 |
|---|---|
| 型・クラス・関数名 | `PascalCase` |
| ローカル変数・引数 | `camelCase` |
| プライベートメンバー変数 | `m_` プレフィックス + `camelCase` |
| 定数・マクロ | `ALL_CAPS` |
| ブレース | Allman スタイル（開き括弧を次行に） |
| インデント | スペース 4 つ |

フォーマット適用: `clang-format -i src/**/*.cpp src/**/*.h`

**Doxygen コメント規約**

ファイル先頭に `@file/@brief/@author/@date`、関数・クラスに `@brief/@param/@return` を記載。宣言側（`.h`）に書き、既存ソースのフォーマットに倣う。

ドキュメント生成: `doxygen Doxyfile`（出力先: `docs/doxygen/html/`）

**STL と Boost は積極的に使用してよい。** マルチプラットフォーム対応が目的のため、プラットフォーム固有 API より STL / Boost の抽象化を優先する（例: `Boost.Process` でプロセス起動、`std::jthread` でスレッド管理）。

## Build System

**Meson** (chosen for Windows/Mac multi-platform support).
- `MyGimpHost` → `shared_library()` (DLL on Windows, dylib on Mac)
- `libgimpwire` / `libgimpbase` → `subproject()` or `dependency()`, dynamically linked
- Platform branching via `host_machine.system()` for Windows vs Mac
- See `docs/spec.md` for the full `meson.build` outline

### ビルド・デプロイコマンド（Windows）

```powershell
# 初回セットアップ（または meson.build 変更後）
meson setup build

# ビルド
meson compile -C build

# デプロイ（CSP プラグインディレクトリへインストール＋ログクリア）
meson install -C build
```

- ビルドディレクトリ: `build/`
- MSVC 環境は PowerShell から実行すると自動アクティベートされる（Bash では cl.exe が見つからないため不可）
- `meson install` 実行後、`cspbridge.log` / `cspbridge_stderr.log` は自動削除される

## Reference Documentation

- GIMP 3.0 API Reference: `developer.gimp.org/resource/api/`
- Wire Protocol overview: `developer.gimp.org/resource/about-plugins/`
- Writing a plug-in tutorial (C samples): `developer.gimp.org/resource/writing-a-plug-in/`

## Implementation Strategy

Per the research doc recommendation: build `libgimpwire` + `libgimpbase` directly into the project unmodified, implement only the host-side stubs for `libgimp`, and start with a single filter plugin prototype using `GIMP_RUN_NONINTERACTIVE` to avoid GUI dialog handling complexity.

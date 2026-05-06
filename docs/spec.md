# CSPBridgeGimp 実装仕様書

## 1. 概要

> **本プロジェクトは技術検証（PoC）を目的としたものであり、有料コンテンツ・商用製品ではありません。**

Clip Studio Paint (CSP) プラグインから GIMP フィルタープラグインをブリッジ実行するホストエミュレーターの実装仕様。CSP プラグインが GIMP ホストを模倣し、GIMP プラグイン EXE を子プロセスとして起動、Wire Protocol でタイル送受信を行い、処理結果を CSP レイヤーに書き戻す。

---

## 2. モジュール構成

```
CSPBridgeGimp/
├── meson.build
├── plugins.json                        # プラグイン定義ファイル（手動管理）
├── config/
│   └── bridge_config.json             # プラグインインストール先などの設定ファイル
├── src/
│   ├── host/
│   │   ├── pdb_stubs.cpp / pdb_stubs.h # PDB プロシージャスタブ群
│   │   └── tile_transfer.cpp / tile_transfer.h  # gimp_tile_get/put 実装
│   ├── ipc/
│   │   ├── process.cpp / process.h     # GIMP プラグイン EXE 起動
│   │   └── wire_io.cpp / wire_io.h     # Wire Protocol I/O・PluginSession
│   ├── config/
│   │   └── config.cpp / config.h       # JSON 設定ファイル読み込み
│   ├── csp/
│   │   ├── plugin_entry.cpp / plugin_entry.h  # CSP エクスポート関数・DllMain
│   │   └── buffer.cpp / buffer.h       # CSP ↔ RGBA 変換
│   └── plugins/                        # プラグイン個別実装（plugins.json エントリと 1:1）
│       ├── plugin_iface.cpp / plugin_iface.h  # 共通インターフェース定義
│       ├── checkerboard.cpp            # plug-in-checkerboard ✓ 実装済み
│       ├── despeckle.cpp               # plug-in-despeckle   ✓ 実装済み
│       └── blinds.cpp                  # plug-in-blinds      ✓ 実装済み
├── tools/
│   └── scanner/
│       └── scan_and_select.py         # GIMPプラグインスキャン＆選択GUIツール
├── scripts/
│   ├── read_config.py                 # meson setup 時に JSON を読んでパスを展開するスクリプト
│   └── list_plugin_ids.py             # plugins.json から id 一覧を stdout に出力するスクリプト
├── build/
│   └── cross/                         # クロスコンパイル用 cross-file テンプレート
├── tests/
│   ├── test_config.cpp
│   ├── test_tile_transfer.cpp
│   ├── test_buffer.cpp
│   ├── test_wire_io.cpp
│   └── test_concurrency.cpp
├── subprojects/
│   ├── nlohmann_json.wrap # nlohmann/json（JSON パーサー、WrapDB 経由）
│   ├── catch2.wrap        # テストフレームワーク（WrapDB 経由）
│   └── packagefiles/
│       └── gimp/          # GIMP wrap 化の将来 TODO 用フォルダ（現フェーズは placeholder のみ）
│   # 注: gimp.wrap / boost.wrap は作成しない方針（§6.1 参照）。
│   #     libgimpwire/libgimpbase は system GIMP 3 の pkg-config から、
│   #     Boost は system インストール（MSYS2 / vcpkg 等）から引く。
└── docs/
    ├── GIMP_Bridge_Summary.md
    ├── gimp3_plugin_availability.md    # GIMP 3 利用可能プラグイン EXE 調査結果
    ├── e2e_debug.md
    ├── spec.md
    └── spec_test.md
```

---

## 3. 設定ファイル仕様（`config/bridge_config.json`）

プラグインのインストール先パスなどの可変設定を JSON で管理する。DLL と同じディレクトリに配置し、起動時に `src/config/config.cpp` が読み込む。

### 3.1 スキーマ

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

実行時にプラットフォームを検出し、対応するセクションのみ読み込む。

| キー | 型 | 説明 |
|---|---|---|
| `windows` / `mac` | `object` | プラットフォーム別設定ブロック |
| `plugin_search_paths` | `string[]` | GIMP プラグイン EXE を探すディレクトリ（順番に検索） |
| `gimp_lib_dir` | `string` | `libgimpwire.dll/dylib` / `libgimpbase.dll/dylib` のあるディレクトリ |
| `csp_plugin_output_dir` | `string` | ビルドした CSP プラグイン（DLL/dylib）のインストール先。プレースホルダー使用可 |

### 3.2 プレースホルダー置換

パス文字列中のプレースホルダーは `load_config()` 内で展開される。`plugin_search_paths` / `gimp_lib_dir` / `csp_plugin_output_dir` すべてに適用。

| プレースホルダー | OS | 展開先の例 |
|---|---|---|
| `%APPDATA%` | Windows | `C:\Users\yourname\AppData\Roaming` |
| `%USERPROFILE%` | Windows | `C:\Users\yourname` |
| `{HOME}` | Mac | `/Users/yourname` |

Windows の `%FOO%` 形式は `ExpandEnvironmentStrings()` で、Mac の `{HOME}` は `getenv("HOME")` で展開する。

```cpp
// 実装例（config.cpp）
static std::string ExpandPlaceholders(const std::string& path)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    ExpandEnvironmentStringsA(path.c_str(), buf, MAX_PATH);
    return buf;
#else
    std::string result = path;
    const char* home = getenv("HOME");
    if (home)
        ReplaceAll(result, "{HOME}", home);
    return result;
#endif
}
```

### 3.3 検索ルール

- `plugin_search_paths` の各ディレクトリを順に走査し、指定プラグイン名に一致する EXE を最初に見つけたものを使用
- パスは絶対パス・相対パス（DLL 基準）どちらも可
- 設定ファイルが存在しない場合はデフォルトパスにフォールバック

| OS | デフォルトパス |
|---|---|
| Windows | `%PROGRAMFILES%\GIMP 3\lib\gimp\3.0\plug-ins` |
| Mac | `/Applications/GIMP.app/Contents/Resources/lib/gimp/3.0/plug-ins` |

### 3.4 実装（`src/config/config.h`）

```cpp
/**
 * @brief Bridge 設定を保持する構造体
 */
struct BridgeConfig
{
    std::vector<std::string> pluginSearchPaths; ///< GIMP プラグイン EXE の検索パス一覧
    std::string gimpLibDir;                     ///< libgimpwire / libgimpbase のディレクトリ
    std::string cspPluginOutputDir;             ///< CSP プラグインのインストール先
};

/**
 * @brief  設定ファイルを読み込み、プレースホルダーを展開して返す
 * @param  configPath  bridge_config.json のファイルパス
 * @return 展開済みの BridgeConfig。ファイル不在時はデフォルト値
 */
BridgeConfig LoadConfig(const std::string& configPath);

/**
 * @brief  設定の検索パスから指定プラグイン EXE を探す
 * @param  cfg         LoadConfig() で取得した設定
 * @param  pluginName  プラグイン名（拡張子なし）
 * @return 見つかった EXE のフルパス。見つからない場合は空文字列
 */
std::string FindPluginExe(const BridgeConfig& cfg, const std::string& pluginName);
```

- JSON パーサーは `subprojects/nlohmann_json.wrap` で **nlohmann/json**（header-only、MIT ライセンス）を取り込む
- `load_config()` はファイル不在時にデフォルト値を返す（例外を投げない）

---

## 4. マルチプラグイン DLL アーキテクチャ

> **2026-05-03 方針転換**: 旧仕様（実行時 `plugins.json` 解析 + 動的 UI 生成）を廃止し、
> **静的プラグイン層アーキテクチャ**に移行した。理由はランタイム生成のバグリスク回避と、
> 既知パラメーターを持つ GIMP プラグインに対してコンパイル時の型安全性を確保するため。

### 4.1 基本方針

CSP は **1エフェクト = 1プラグイン DLL** の構造を持つ。各 GIMP プラグインに対応する
C++ 実装ファイル（`src/plugins/*.cpp`）を事前に用意し、ビルド時に選択する。
**実行時の `plugins.json` 解析は行わない**。

```
plugins.json（ビルド選択リスト）
    ↓ meson setup 時に読み込み
meson.build
    ↓ foreach でプラグイン数だけ shared_library() を生成
CSPBridgeGimp_PlugInCheckerboard.cpm  ← checkerboard.cpp をリンク
CSPBridgeGimp_GaussBlur.cpm           ← gauss_blur.cpp をリンク（将来）
CSPBridgeGimp_Pixelize.cpm            ← pixelize.cpp をリンク（将来）
```

### 4.2 層構成

| 層 | 役割 | ファイル |
|---|---|---|
| **CSP エントリ（コア）** | `TriglavPluginCall`・CSP ライフサイクル全般・`DbgLog`・`GetModuleDir` | `src/csp/plugin_entry.cpp` |
| **プラグイン層（薄いラッパー）** | GIMP 固有の EXE 名・プロシージャ名・CSP UI 定義・args 組み立て | `src/plugins/<id>.cpp` |
| **Wire Protocol / Tile** | GIMP プロセス起動・タイル送受信 | `src/ipc/`・`src/host/` |

コアは `GetPluginInfo()` でメタデータを取得し、`SetupProperty()` / `BuildFilterParams()` /
`OnPropertyChanged()` をプラグインに委譲するだけで、SDK 型に対する dispatch ロジックを持たない。
各 `src/plugins/*.cpp` がその知識をカプセル化する。

### 4.3 plugin_iface.h — プラグイン層インターフェース（CSPBridge アライン版）

> **2026-05-04 方針確定**: variant + visit による中央 dispatch 案は廃止し、
> CSPBridge `Effects/Samples/Blur.cs` `HSV.cs` の流儀に合わせて
> **各プラグインが SDK を直接呼ぶ**設計に変更。理由:
>   - core が SDK 型ごとの dispatch を持たないことで一段シンプル
>   - 新型（Decimal / Boolean / Enumeration / String / Point 等）を追加するときに
>     core を触らずプラグイン側だけで完結
>   - C# 版 CSPBridge と完全にアラインしているため共通理解しやすい

各 `src/plugins/*.cpp` は以下の **4 つの自由関数**をリンク時に提供する（仮想関数不要・1 DLL に 1 プラグイン）。

ヘッダー: **`src/plugins/plugin_iface.h`**

```cpp
/**
 * @file   plugin_iface.h
 * @brief  プラグイン層インターフェース定義（CSPBridge アライン版）
 * @author CSPBridgeGimp
 * @date   2026-05-04
 */
#pragma once
#include <string>
#include <vector>
#include "../ipc/wire_io.h"   // FilterParams, GpParam
#include "TriglavPlugInDefine.h"
#include "TriglavPlugInService.h"

/// @brief プラグイン基本情報（メタデータ）
struct PluginInfo
{
    std::string exeName;        ///< GIMP プラグイン EXE 名（拡張子なし）
    std::string procName;       ///< GIMP プロシージャ名
    std::string displayName;    ///< CSP UI フィルター表示名
    std::string category = "GIMP Bridge"; ///< CSP UI カテゴリ
    bool        canPreview = false;
    /// 対応カラーモード。既定: RGBA + GrayAlpha （buffer.cpp 対応範囲）
    std::vector<int> targetKinds = {
        kTriglavPlugInFilterTargetKindRasterLayerRGBAlpha,
        kTriglavPlugInFilterTargetKindRasterLayerGrayAlpha,
    };
};

/// @brief プラグイン基本情報を返す
PluginInfo GetPluginInfo();

/// @brief FilterInitialize 時にプロパティ UI を SDK 直接呼び出しで構築する
/// プラグインが addItemProc / setIntegerValueProc 等を直接呼ぶ
/// （CSPBridge Blur.cs::FilterInitialize と同パターン）
/// PropertyService2 は Enumeration / String 用。NULL の可能性ありで分岐は呼び出し側の責務
void SetupProperty(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInStringService*     strSvc,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  propSvc2);

/// @brief FilterRun 時に CSP プロパティ値から GIMP wire パラメーターを組み立てる
FilterParams BuildFilterParams(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  propSvc2);

/// @brief プロパティ変更通知のフック
/// 戻り値は kTriglavPlugInPropertyCallBackResultNoModify / Modify / Invalid
/// 多くの PoC フィルターは NoModify 固定で十分
TriglavPlugInInt OnPropertyChanged(
    TriglavPlugInPropertyObject     propObj,
    TriglavPlugInInt                itemKey,
    TriglavPlugInInt                notify,
    TriglavPlugInPropertyService*   propSvc,
    TriglavPlugInPropertyService2*  propSvc2);

/// @brief ASCII 文字列から TriglavPlugInStringObject を生成（共通ヘルパー）
/// 使用後に strSvc->releaseProc が必須
TriglavPlugInStringObject CreateAsciiString(
    TriglavPlugInStringService* strSvc, const std::string& text);
```

#### plugin_entry.cpp の役割

`FilterInitialize` セレクター:
1. `GetPluginInfo()` でメタデータを取得し、CSP の Category / FilterName / CanPreview / TargetKinds を設定
2. `propSvc->createProc(&propObj)` でプロパティオブジェクトを確保
3. `SetupProperty(propObj, strSvc, propSvc, propSvc2)` をプラグインに委譲（UI 構築）
4. `setProperty` / `setPropertyCallBack` を実行

`FilterPropertyCallBack` 静的サンク:
- `BridgeData` から PropertyService v1/v2 を取り出し、`OnPropertyChanged()` の戻り値を
  そのまま CSP に返す

`FilterRun` セレクター:
- `BuildFilterParams(propObj, propSvc, propSvc2)` でプラグインに wire 引数組立を委譲
- 残りはタイル転送・GIMP プロセス起動・書き戻しの共通ロジック

`GIMP_PLUGIN_EXE` / `GIMP_PLUGIN_PROC` マクロは廃止。`GIMP_PLUGIN_ID` のみ残す（CSP moduleId 用）。

#### SDK 型ごとのセッター早見表

| 型 | addItemProc 値型 | service 層 | 値セット | Min/Max/Default |
|---|---|---|---|---|
| Boolean   | `Boolean (0x01)`     | v1 | `setBooleanValueProc`     | なし（値のみ） |
| Integer   | `Integer (0x11)`     | v1 | `setIntegerValueProc`     | `setInteger{Default,Min,Max}ValueProc` |
| Decimal   | `Decimal (0x12)`     | v1 | `setDecimalValueProc`     | `setDecimal{Default,Min,Max}ValueProc` |
| Enumeration | `Enumeration (0x02)` | **v2** | `setEnumerationValueProc` | `addEnumerationItemProc` で各選択肢追加 |
| String    | `String (0x31)`      | **v2** | `setStringValueProc`      | `setStringDefaultValueProc` / `setStringMaxLengthProc` |

### 4.4 プラグイン実装例（`src/plugins/checkerboard.cpp`）

```cpp
#include "plugin_iface.h"

namespace { constexpr int ItemKeyPsychobilly = 1; constexpr int ItemKeyCheckSize = 2; }

PluginInfo GetPluginInfo()
{
    return {
        .exeName     = "checkerboard",
        .procName    = "plug-in-checkerboard",
        .displayName = "Checkerboard",
    };
}

void SetupProperty(propObj, strSvc, propSvc, /*propSvc2*/)
{
    // psychobilly (Boolean)
    auto psyLabel = CreateAsciiString(strSvc, "Psychobilly");
    propSvc->addItemProc(propObj, ItemKeyPsychobilly,
        kTriglavPlugInPropertyValueTypeBoolean,
        kTriglavPlugInPropertyValueKindDefault,
        kTriglavPlugInPropertyInputKindDefault,
        psyLabel, '\0');
    propSvc->setBooleanValueProc(propObj, ItemKeyPsychobilly, kTriglavPlugInBoolFalse);
    strSvc->releaseProc(psyLabel);

    // Check Size (Integer slider)
    auto szLabel = CreateAsciiString(strSvc, "Check Size");
    propSvc->addItemProc(propObj, ItemKeyCheckSize,
        kTriglavPlugInPropertyValueTypeInteger, ...,
        szLabel, '\0');
    propSvc->setIntegerValueProc       (propObj, ItemKeyCheckSize, 10);
    propSvc->setIntegerDefaultValueProc(propObj, ItemKeyCheckSize, 10);
    propSvc->setIntegerMinValueProc    (propObj, ItemKeyCheckSize, 1);
    propSvc->setIntegerMaxValueProc    (propObj, ItemKeyCheckSize, 200);
    strSvc->releaseProc(szLabel);
}

FilterParams BuildFilterParams(propObj, propSvc, /*propSvc2*/)
{
    TriglavPlugInBool psy = kTriglavPlugInBoolFalse;
    propSvc->getBooleanValueProc(&psy, propObj, ItemKeyPsychobilly);
    TriglavPlugInInt sz = 10;
    propSvc->getIntegerValueProc(&sz, propObj, ItemKeyCheckSize);

    FilterParams params;
    params.procedureName = "plug-in-checkerboard";
    params.args = {
        GpParam{GpParamType::Int, "gboolean", "", psy ? 1 : 0,        0.0},
        GpParam{GpParamType::Int, "gint",     "", static_cast<int>(sz), 0.0},
    };
    return params;
}

TriglavPlugInInt OnPropertyChanged(...)
{
    return kTriglavPlugInPropertyCallBackResultNoModify;
}
```

新プラグインを追加する際はこのファイルをテンプレートに `src/plugins/<id>.cpp` を作成し、
`scan_and_select.py` で選択してから `meson setup --reconfigure build` を実行する。

### 4.5 plugins.json スキーマ（ビルド選択リスト）

`tools/scanner/scan_and_select.py` が `src/plugins/` をスキャンして生成する。
**実行時には使用しない**（meson.build の入力のみ）。

```json
[
  {
    "id":  "PlugInCheckerboard",
    "src": "src/plugins/checkerboard.cpp"
  }
]
```

| フィールド | 内容 |
|---|---|
| `id`  | DLL 名サフィックス兼 `GIMP_PLUGIN_ID`（ASCII、スペースなし） |
| `src` | `src/plugins/` 以下の相対パス（meson.build が `files()` に渡す） |

`plugins.json` はリポジトリに含めてよい。別プラグインを追加・除外したい場合は `scan_and_select.py` を再実行する。

### 4.6 meson.build プラグインループ（変更後）

`list_plugin_ids.py` は `id|src` 形式を出力する（旧 `id|exe|proc` から変更）。

```meson
foreach entry : plugin_ids
    fields     = entry.split('|')
    plugin_id  = fields[0]   # 例: PlugInCheckerboard
    plugin_src = fields[1]   # 例: src/plugins/checkerboard.cpp
    if is_msvc
        macro_id = '/DGIMP_PLUGIN_ID="' + plugin_id + '"'
    else
        macro_id = '-DGIMP_PLUGIN_ID="' + plugin_id + '"'
    endif
    shared_library(
        'CSPBridgeGimp_' + plugin_id,
        [files('src/csp/plugin_entry.cpp'), files(plugin_src)],
        cpp_args:     [macro_id],           # GIMP_PLUGIN_EXE / PROC マクロは不要
        dependencies: common_deps,
        link_with:    core_lib,
        include_directories: include_directories('extern/TriglavPlugInSDK'),
        name_suffix:  'cpm',
        install:      true,
        install_dir:  csp_output_dir,
    )
endforeach
```

### 4.7 CSPBridge との対応関係

| CSPBridge | CSPBridgeGimp |
|---|---|
| `EFFECT_ID="Blur"` | `GIMP_PLUGIN_ID="GaussBlur"` |
| `CSPBridgeEffects.dll`（C# 共通処理） | `src/csp/plugin_entry.cpp`（コア） |
| `CSPBridgeBlur.dll` / `CSPBridgeHSV.dll` | `src/plugins/gauss_blur.cpp` / `src/plugins/unsharp.cpp` |

---

## 5. プラグインスキャナーツール（`tools/scanner/scan_and_select.py`）

> **2026-05-03 役割変更**: 旧仕様（GIMP インストールをスキャンして `plugins.json` を生成）から
> **ビルド選択ツール**（`src/plugins/` に実装済みのプラグインから選択して `plugins.json` を生成）に変更。
> GIMP インストールのスキャン・探索は開発時専用ツール `tools/dev/discover_gimp_plugins.py` が担う。

### 5.1 役割と位置づけ

`meson setup` の**前に手動で実行**する独立ツール。`plugins.json`（§4.5）が唯一の出力成果物であり、以降のビルドはこのファイルに依存する。

```
【通常ビルドフロー】
① scan_and_select.py を実行（手動・プラグイン追減時のみ）
        ↓  plugins.json を保存
② meson setup --reconfigure build
③ meson compile -C build
④ meson install -C build

【新プラグイン追加フロー】
① tools/dev/discover_gimp_plugins.py で GIMP をスキャン（プロシージャ名・パラメーター確認）
② src/plugins/<id>.cpp を実装（plugin_iface.h に従って3関数を実装）
③ scan_and_select.py で新プラグインを選択 → plugins.json 更新
④ meson setup --reconfigure build → compile → install
```

### 5.2 処理フロー（新仕様）

```
1. src/plugins/ を走査して *.cpp ファイルを列挙（plugin_iface.h を除く）
2. 各ファイルから plugin_id を推定（ファイル名 → PascalCase 変換、または
   `re` モジュールで `GetPluginInfo()` の return 文を静的テキストパース）
3. tkinter チェックリストダイアログで一覧表示
4. ユーザーがチェックを入れたプラグインのみ plugins.json に書き出す
   （§4.5 形式: id + src パス）
```

### 5.3 起動引数とメッセージ（GIMP 3.0 実装準拠）

`libgimp/gimp.c` の `gimp_main()` は 7 要素の argv を要求する：

```
argv[0] = progname
argv[1] = "-gimp"
argv[2] = <protocol_version>    10 進文字列（GIMP_3_0 = "277"）
argv[3] = <read_fd>             子が読む fd
argv[4] = <write_fd>            子が書く fd
argv[5] = <mode>                "-query" | "-init" | "-run"
argv[6] = <stack_trace>         "0" (NEVER) / "1" (QUERY) / "2" (ALWAYS)
```

`-query` モードで使用するメッセージ型番号（`libgimpbase/gimpprotocol.h`）:

| 型 | 番号 | 方向 | 役割 |
|---|---|---|---|
| `GP_QUIT`           | 0  | 双方向 | セッション終了 |
| `GP_PROC_RUN`       | 5  | プラグイン → スキャナー | クエリ中の PDB コールバック |
| `GP_PROC_RETURN`    | 6  | スキャナー → プラグイン | PDB コールバックの成功応答 |
| `GP_PROC_INSTALL`   | 9  | プラグイン → スキャナー | プロシージャ登録（引数定義） |
| `GP_PROC_UNINSTALL` | 10 | プラグイン → スキャナー | 登録解除通知（クエリ時は通常不要） |
| `GP_HAS_INIT`       | 12 | プラグイン → スキャナー | init ハンドラ有無通知 |

> **重要**: GIMP 2.x には `GP_QUERY` メッセージが存在したが、**GIMP 3.0 では削除**されている。クエリ開始は argv の `-query` フラグで行う。本仕様書の当初記述（`GP_QUERY` を送信）は誤りであり、本節が正。

run フェーズ（`GP_TILE_REQ` / `GP_TILE_DATA` 等）は**不要**。

**ワイヤーフォーマット**（`libgimpbase/gimpwire.c` / `gimpprotocol.c`）:

- すべての整数は**ネットワークバイトオーダー（big-endian）**
- メッセージ = `uint32 type` + 型ごとのペイロード（**全体長フィールドなし**）
- 文字列 = `uint32 length_including_NUL` + `length bytes`（末尾 NUL 含む）。`length == 0` は NULL

**GP_PROC_INSTALL ペイロード**:

```
string  name
uint32  proc_type
uint32  n_params
uint32  n_return_vals
GPParamDef[n_params]
GPParamDef[n_return_vals]
```

**GPParamDef**:

```
uint32  param_def_type   (GPParamDefType enum)
string  type_name
string  value_type_name
string  name
string  nick
string  blurb
uint32  flags
<param_def_type 依存 meta>  例: INT は min/max/default の int64×3
```

### 5.4 PDB コールバックの扱い

GIMP 3.0 では、プラグインはクエリ中に次の PDB 手続きを `GP_PROC_RUN` として呼び出してメタデータを送ってくる：

| PDB procedure | 引数（文字列は簡略化） | 取り出し先 |
|---|---|---|
| `gimp-pdb-set-proc-menu-label`   | `(proc_name, menu_label)` | `menu_label` |
| `gimp-pdb-add-proc-menu-path`    | `(proc_name, menu_path)`  | `menu_paths[]` |
| `gimp-pdb-set-proc-documentation`| `(proc_name, blurb, help, help_id)` | `blurb` / `help` / `help_id` |
| `gimp-pdb-set-proc-attribution`  | `(proc_name, authors, copyright, date)` | `authors` / `copyright` / `date` |

スキャナーは中身を解釈しつつ、プラグインにはすべて空の成功レスポンスを返す:

```
GP_PROC_RETURN ペイロード:
    string  name       ← 呼び出された procedure 名をそのまま返す
    uint32  n_params   = 1
    uint32  param_type = GP_PARAM_TYPE_INT (=0)
    string  type_name  = "GimpPDBStatusType"
    int32   d_int      = GIMP_PDB_SUCCESS (=0)
```

本ツールで MyGimpHost（C++ 版）を実装する際、この PDB コールバック応答ルールは `pdb_stubs.cpp` でも踏襲する必要がある。

### 5.5 実装言語・依存

| 項目 | 選択 |
|---|---|
| 言語 | Python 3.10+ |
| GUI | tkinter（標準ライブラリ、追加依存なし） |
| Wire Protocol | `subprocess` + `struct`（バイナリパック） |
| JSON 出力 | `json`（標準ライブラリ） |

### 5.6 FD 継承（Windows / Mac）

子プロセスに read/write fd を引き渡す方法。**2026-04-22 GIMP 3.2.4 実機で Windows 版を疎通確認済み**（118 プラグイン中 114 成功、残 4 は外部依存欠如による failure で protocol 問題ではない）。

#### POSIX (Mac / Linux)

`os.pipe()` → `os.set_inheritable(fd, True)` → `subprocess.Popen(..., close_fds=True, pass_fds=(...))`。POSIX 標準手順で確実に動作する。

#### Windows — GIMP 本体互換方式（採用）

`app/plug-in/gimpplugin.c`（GIMP 本体 host 側）が使う方式をそのまま再現する。Python の `subprocess.Popen` では `lpReserved2` を扱えないため、`ctypes` から `CreateProcessW` を**直接呼ぶ**。

手順:

1. **binary パイプ作成**: `os.pipe()` + `msvcrt.setmode(fd, os.O_BINARY)` で fd を binary mode に固定。GIMP 側は `_pipe(fds, 4096, _O_BINARY)` 相当
2. **CLOEXEC**: 親側 fd の HANDLE は `SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)` で継承不可にする。子側 fd は継承可に
3. **`STARTUPINFO.lpReserved2`** に MSVCRT fd-inherit ブロックを埋め、`CreateProcessW(bInheritHandles=TRUE)` を呼ぶ。子の MinGW/MSVCRT は起動時にこのブロックを読み、fd テーブルを再構築する
4. argv には子側の fd 番号（例: "3", "4"）を渡す。`libgimp/gimp.c` の `gimp_main()` が `atoi(argv[3])` / `atoi(argv[4])` で fd を取得

lpReserved2 ブロックのレイアウト（MSVCRT 内部形式、公式ドキュメント非公開）:

```
offset  size        content
------  ----------  -----------------------------------------------------
0       uint32      count (= 登録する fd の総数)
4       uint8 * count  各 fd の ioinfo フラグ
                       FOPEN=0x01 / FEOFLAG=0x02 / FCRLF=0x04 /
                       FPIPE=0x08 / FNOINHERIT=0x10 / FAPPEND=0x20 /
                       FDEV=0x40 / FTEXT=0x80
4+count HANDLE * count  各 fd の Win32 HANDLE (64-bit で 8 bytes/個)
                        未割当 fd は INVALID_HANDLE_VALUE (-1) + flags=0
```

**フラグ選択**: binary パイプは `FOPEN | FPIPE` (= 0x09)。`FTEXT` は**立てない**（立てると子側で CRLF 変換が入り wire protocol が壊れる）。

**fd 割り当て慣行**:
- 子 fd 0/1/2（std streams）: INVALID_HANDLE + flags=0 を入れ、子は `STARTUPINFO.hStdInput/Output/Error` を使う
- 子 fd 3: プラグインが read する fd（親が書く側の反対端）
- 子 fd 4: プラグインが write する fd
- argv: `["plug-in.exe", "-gimp", "279", "3", "4", "-query", "0"]`

参考実装: `tools/scanner/scan_and_select.py` の `_spawn_plugin_windows()` 関数と `_build_msvcrt_inherit_block()` ヘルパ。C++ 版 `MyGimpHost` (`src/ipc/process.cpp`) でも同じ方式を移植する。

#### 検証済みでない構成
- **Python`subprocess.Popen` の stdin/stdout 経由**: 原理的には動くが子 CRT の text mode で `\n → \r\n` 変換が入り、wire protocol が壊れる。CRLF 逆変換レイヤーで潰す hack は存在するが fragile。採用しない。
- **named pipe**: argv に fd 整数を要求する `gimp_main` の仕様と両立しない。
- **GIMP 側パッチ**: LGPL 要件上差分公開が発生。ROI が低いため採用しない。

### 5.7 MyGimpHost との関係

スキャナーの Wire Protocol クエリフェーズ実装は、`MyGimpHost` の**最初のプロトタイプ**となる。スキャナーで Wire Protocol の疎通を確認してから、`MyGimpHost` に run フェーズ（タイル転送）を追加する順序で進める。

**スキャナーから MyGimpHost へ移植すべき知見**（2026-04-22 実機検証済み事項含む）:

- **プロトコルバージョン**: GIMP 3.2 = `0x0117` (= 279)。GIMP 3.0 の `0x0115` (= 277) とは非互換。子プラグイン側は引数 `<protocol_version>` を `atoi` してバージョン一致をチェックし、mismatch なら `"GIMP is using an older version of the plug-in protocol."` で即終了する
- **enum 追加（3.0 → 3.2）**: `GP_PARAM_DEF_TYPE_CURVE = 14`（meta は `uint32 none_ok`）、`GP_PARAM_TYPE_CURVE = 15`。既存の scanner/host は両方にハンドラを持たないと CURVE パラメータを持つプラグインで desync
- **argv 7 要素フォーマット**: `[<progname>, "-gimp", "<protocol_version>", "<read_fd>", "<write_fd>", "<mode>", "<stack_trace>"]`
- **`-query` モード挙動**: プラグインは `GP_CONFIG` を待たず即 `GP_PROC_INSTALL` を書く
- **PDB コールバック経由のメタデータ**: `menu_label` / `blurb` / `attribution` / `documentation` / `image-types` / `sensitivity-mask` 等は `GP_PROC_INSTALL` に含まれず、`GP_PROC_RUN` で送られる。host は各呼び出しに対し **`GP_PROC_RETURN` + `status=GIMP_PDB_SUCCESS`** を同期応答する必要あり。未応答だと子がブロックする
- **メッセージフレーミング**: ヘッダーは uint32 type のみ（長さ無し）。未知 type が来ると sync 喪失。受信側は**全 type を網羅**した読み捨て／応答ロジックを持つこと
- **DLL 解決**: Windows では `libgimp-3.0-0.dll`, `libgimpbase-3.0-0.dll` 等が `gimp_lib_dir` にあるため、子プロセスの `PATH` 先頭にこのディレクトリを追加しないと `STATUS_DLL_NOT_FOUND` (0xC0000135) で即クラッシュする
- **Windows FD 継承**: §5.6 参照。`CreateProcessW` + `STARTUPINFO.lpReserved2` で MSVCRT fd テーブルを明示的に引き継ぐ必要がある。`subprocess.Popen` / `posix_spawn` 標準ルートではダメ

---

## 6. ビルドシステム（Meson）

### 6.1 依存ライブラリの取り込み方針（PoC フェーズ）

| 依存 | 取り込み方法 | 備考 |
|---|---|---|
| `nlohmann_json` | Meson WrapDB（`subprojects/nlohmann_json.wrap`） | `meson wrap install nlohmann_json` で取得。MIT |
| `catch2` | Meson WrapDB（`subprojects/catch2.wrap`） | `meson wrap install catch2` で取得。BSL-1.0 |
| `boost` | **system インストール前提**（pkg-config / cmake） | WrapDB にフル Boost エントリ無し。MSYS2 / vcpkg / Homebrew 等で導入し、`dependency('boost', modules: ['filesystem', 'system'], required: false)` で引く。見つからない場合は warning のみで setup 継続 |
| `libgimpwire` / `libgimpbase` | **system インストール済み GIMP 3 を pkg-config 経由**で参照 | GIMP の meson.build は本体フルビルドを要求するため PoC では wrap 化しない。`dependency('gimp-wire-3.0', required: false)` → fallback で `libgimpwire-3.0` の順に試し、見つからない場合は warning のみで setup 継続 |

**TODO（将来フェーズ）**:

- `subprojects/gimp.wrap`（wrap-git）+ `subprojects/packagefiles/gimp/meson.build` overlay を整備し、`libgimpbase/*.c` と `libgimpwire/*.c` のみを `static_library()` として直接ビルドする。overlay の配置場所は `subprojects/packagefiles/gimp/` を確保済み（README.txt で有効化手順を明文化）。
- Boost も本格運用では WrapDB に独自 boost wrap をミラーする、もしくは `b2` 統合の自作 wrap を作るか検討する。

- プラグイン DLL は `plugins.json` の各エントリに対して `shared_library()` を生成（Windows: DLL、Mac: dylib）
- プラットフォーム分岐は `host_machine.system()` で判定（対象: Windows / Mac のみ）

### 6.2 meson.build 抜粋（実装と整合）

> **MSVC 注意**: MSVC は `cpp_std=c++23` を `default_options` で受け付けない（`c++latest` が正しい値）。
> `default_options` から `cpp_std` を外し、コンパイラ検出後に `add_project_arguments` で分岐する。
> GCC/Clang: `-std=c++23`、MSVC: `/std:c++latest`。実機検証済み（VS 19.50）。
> また `files()` は存在しないファイルでエラーになるため、`fs.exists()` で段階的に追加する。

```meson
# meson.build（概略）
project('CSPBridgeGimp', 'cpp', version: '0.1.0')  # cpp_std は下記で分岐設定

# config/bridge_config.json から install 先を取得
py = import('python').find_installation('python3', required: true)
config_reader = files('scripts/read_config.py')
platform = host_machine.system()   # 'windows' or 'darwin'

csp_output_dir = run_command(
  py, config_reader,
  '--config', meson.project_source_root() / 'config/bridge_config.json',
  '--platform', platform,
  '--key', 'csp_plugin_output_dir',
  check: true,
).stdout().strip()

# WrapDB 経由で取得できるもの
nlohmann_json_dep = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])
catch2_dep        = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])

# system インストール前提（見つからなくても setup は継続）
boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: false)

libgimpwire_dep = dependency('gimp-wire-3.0', required: false)
if not libgimpwire_dep.found()
  libgimpwire_dep = dependency('libgimpwire-3.0', required: false)
endif

libgimpbase_dep = dependency('gimp-base-3.0', required: false)
if not libgimpbase_dep.found()
  libgimpbase_dep = dependency('libgimpbase-3.0', required: false)
endif

common_deps = [nlohmann_json_dep]
if boost_dep.found()
  common_deps += boost_dep
endif
if libgimpwire_dep.found()
  common_deps += libgimpwire_dep
endif
if libgimpbase_dep.found()
  common_deps += libgimpbase_dep
endif

srcs = files(
  'src/config/config.cpp',
  'src/host/host.cpp',
  'src/host/pdb_stubs.cpp',
  'src/host/tile_transfer.cpp',
  'src/ipc/process.cpp',
  'src/ipc/wire_io.cpp',
  'src/csp/plugin_entry.cpp',
  'src/csp/buffer.cpp',
)

# plugins.json から id 一覧を取得し、プラグインごとに DLL を生成
# （plugins.json が無い場合は warning でスキップ。scan_and_select.py を先に実行する）
plugin_ids = run_command(
  py, 'scripts/list_plugin_ids.py', 'plugins.json',
  check: false,
).stdout().strip().split('\n')

foreach plugin_id : plugin_ids
  shared_library(
    'CSPBridgeGimp_' + plugin_id,
    srcs,
    cpp_args: ['-DGIMP_PLUGIN_ID="' + plugin_id + '"'],
    dependencies: common_deps,
    install: true,
    install_dir: csp_output_dir,
  )
endforeach
```

`build/read_config.py` は JSON を読んでプレースホルダーを展開し、指定キーの値を stdout に出力する小さなスクリプト:

```python
# build/read_config.py
import argparse, json, os

parser = argparse.ArgumentParser()
parser.add_argument('--config')
parser.add_argument('--platform')   # 'windows' or 'darwin'
parser.add_argument('--key')
args = parser.parse_args()

with open(args.config, encoding='utf-8') as f:
    cfg = json.load(f)

platform_key = 'mac' if args.platform == 'darwin' else 'windows'
value = cfg[platform_key][args.key]

# Windows: %FOO% を環境変数展開、Mac: {HOME} を展開
value = os.path.expandvars(value)
value = value.replace('{HOME}', os.path.expanduser('~'))

print(value)
```

- クロスコンパイル用 cross-file テンプレートは `build/cross/` 以下に配置
- `scripts/read_config.py` は `meson setup` 時に実行されるため Python 3 が必須

---

## 7. SIMD / Intrinsics 方針

タイル転送・バッファ変換（RGBA ↔ CSP フォーマット）のピクセル処理に SSE/NEON を活用する。

### 5.1 使用ヘッダー

```cpp
#if defined(_M_X64) || defined(__x86_64__)
  // Windows (x64)
  #include <immintrin.h>   // AVX2 以下すべてを包含
  #include <emmintrin.h>   // SSE2（フォールバック用）
#elif defined(__aarch64__)
  // Mac Apple Silicon (M1/M2/M3)
  #include <arm_neon.h>
#endif
```

### 5.2 プラットフォーム対応

| プラットフォーム | アーキテクチャ | 使用する命令セット |
|---|---|---|
| Windows | x86-64 | SSE2 / SSE4.1 / AVX2 |
| Mac | ARM64 (Apple Silicon) | NEON（`arm_neon.h`） |

Mac は Apple Silicon（ARM64）のみを対象とする。Intel Mac は対象外。

### 5.3 Meson でのフラグ設定

```meson
cpu = host_machine.cpu_family()   # 'x86_64' or 'aarch64'
compiler = meson.get_compiler('cpp')

if cpu == 'x86_64'
  if compiler.get_id() == 'msvc'
    add_project_arguments('/arch:AVX2', language: 'cpp')
  else
    add_project_arguments('-mavx2', '-msse4.1', language: 'cpp')
  endif
elif cpu == 'aarch64'
  add_project_arguments('-march=armv8-a+simd', language: 'cpp')
endif
```

---

## 8. IPC レイヤー仕様（`src/ipc/`）

### 6.1 子プロセス起動

**Boost.Process は使用しない**。GIMP プラグインの Windows 起動には
`STARTUPINFO.lpReserved2` に MSVCRT fd-inherit ブロックを埋め込む必要があり、
Boost.Process ではこれができないことが実機検証で確認されている（2026-04-22 確認済み）。

PoC 現フェーズでは Boost.Process を利用不可として扱い、OS 別に直接実装する。

| OS | 実装方式 |
|---|---|
| Windows | `CreateProcessW` + `STARTUPINFO.lpReserved2`（MSVCRT fd-inherit ブロック） |
| Mac | `fork` + `execv` + POSIX pipes（`dup2` で fd を 3/4 に移動） |

実装は `tools/scanner/scan_and_select.py` の `_spawn_plugin_windows()` /
`_spawn_plugin_posix()` / `_build_msvcrt_inherit_block()` を C++ に移植したもの。
詳細は `src/ipc/process.cpp` を参照。

**公開 API（`src/ipc/process.h`）**:

```cpp
/// GIMP プラグインモード
enum class PluginMode { Query, Run };

/// 起動中のプロセスを保持する構造体
struct PluginProcess
{
    int readFd;   ///< ホストがプラグインから読む fd（親側）
    int writeFd;  ///< ホストがプラグインへ書く fd（親側）
#ifdef _WIN32
    HANDLE m_hProcess;
    HANDLE m_hThread;
#else
    pid_t m_pid;
#endif
};

/// 起動（失敗時は std::runtime_error を投げる）
PluginProcess SpawnPlugin(
    const std::string& exePath,
    const std::string& gimpLibDir,
    PluginMode mode,
    int protocolVersion = 0x0117);   // GIMP 3.2 = 279

int  WaitPlugin(PluginProcess& proc, int timeoutMs = -1);
void TerminatePlugin(PluginProcess& proc);
void ClosePlugin(PluginProcess& proc);
```

- readFd / writeFd の close は呼び出し側（wire_io 層）の責務
- 子プロセス終了検知: パイプ読み取りで 0 バイト（EOF）→ 子がクラッシュまたは正常終了
- 子への argv: `<progname> -gimp <protocolVersion> 3 4 <-query|-run> 0`
- Windows の PATH 追加・Mac の DYLD_LIBRARY_PATH 追加は SpawnPlugin 内部で行う

### 6.2 Wire Protocol 通信（`wire_io.cpp`）

**スレッドモデル: 子プロセス 1 本につき 1 スレッド**

- GIMP プラグイン EXE を起動するたびに `std::jthread` を生成し、そのスレッドがパイプと Wire Protocol の送受信を専任する
- 複数フィルターの並列実行が可能（スレッドごとに独立したパイプを持つ）
- `libgimpwire` の `gimp_wire_read()` / `gimp_wire_write_msg()` を使用

**共有リソースの保護**

| 共有リソース | 保護手段 |
|---|---|
| CSP 画像バッファ（読み取り） | `std::shared_mutex` で複数スレッドの同時読み取りを許可 |
| CSP 画像バッファ（書き戻し） | `std::unique_lock` で排他書き込み |
| タイルバッファ | `thread_local` — 各スレッドが独立して保持、共有なし |

```cpp
/**
 * @brief GIMP プラグイン 1 プロセスとの通信セッションを管理するクラス
 *
 * 子プロセス 1 本につき 1 インスタンスを生成し、std::jthread で
 * Wire Protocol の送受信を専任する。
 */
class PluginSession
{
public:
    /**
     * @brief  コンストラクター。子プロセスを起動してスレッドを開始する
     * @param  exePath  GIMP プラグイン EXE のフルパス
     */
    explicit PluginSession(const std::string& exePath);

    /**
     * @brief  フィルターを非同期実行する
     * @param  params  フィルターパラメーター
     * @return 処理完了を通知する std::future
     */
    std::future<void> RunFilter(const FilterParams& params);

    /// デストラクター。m_worker スレッドを安全に終了する
    ~PluginSession();

private:
    std::jthread m_worker; ///< Wire Protocol 送受信専任スレッド
    // パイプは m_worker スレッドが専任するため mutex 不要
    // 共有バッファの保護は tile_transfer / pdb_stubs 側で行う
};
```

- 子プロセス異常終了の検知: パイプ読み取りが 0 バイト（EOF）→ `std::stop_token` でスレッドに停止通知
- スレッド間のエラー伝播は `std::promise` / `std::future` で行う
- `RunFilter()` は **1 セッションにつき 1 回のみ**呼び出し可（内部 `std::promise` が 1 回限り）。複数フィルターを連続適用する場合は `PluginSession` を破棄して新たに生成すること
- `RunFilter()` を呼ぶ前にデストラクタを呼んでも安全（worker が `stop_token` を検知して正常退場する）
- `GP_PROC_RETURN` と `GP_PROC_RUN` のペイロード形式は同一（GIMP Wire Protocol 仕様）: `string name, uint32 n_params, GPParam[n_params]`

### 6.3 GP_PROC_RUN 送信フォーマット（-run モード、2026-05-01 上流確認済み）

確認ソース: `libgimpbase/gimpprotocol.c` `_gp_proc_run_write` / `_gp_params_write`, `libgimp/gimpgpparams-body.c` `gimp_value_to_gp_param`, `app/plug-in/gimppluginmanager-call.c` `gimp_plug_in_manager_call_run`。

#### ホスト → プラグインのメッセージシーケンス（-run モード）

```
host   → plugin: GP_CONFIG    (run モード開始)
host   → plugin: GP_PROC_RUN  (フィルター呼び出し)
```

**重要**: プラグインは GP_CONFIG 受信後に何も送らずそのまま GP_PROC_RUN を待つ。GP_HAS_INIT は `-query` モードでのみ送られ、`-run` モードでは不要。(`libgimp/gimpplugin.c` `gimp_plug_in_loop` にて確認。)

#### GP_CONFIG ペイロードフォーマット（GIMP 3.2、2026-05-02 上流確認済み）

確認ソース: `libgimpbase/gimpprotocol.c` `_gp_config_read` / `_gp_config_write`、`libgimpbase/gimpwire.c` `_gimp_wire_read_gegl_color`、`libgimp/gimp.c` `_gimp_config`。

`-query` モードではプラグインが GP_CONFIG を**読まない**ため payload が不完全でもクラッシュしないが、`-run` モードでは `_gp_config_read` が下記 27 フィールドを順に読むため、**完全に揃えて送る必要がある**。

`protocol_version` は **payload に含まれない**。`argv[2]` で渡し済み。

```
[1] tile_width             int32
[2] tile_height            int32
[3] shm_id                 int32   (PoC では -1 = shm 不使用)
[4] check_size             int8
[5] check_type             int8
[6] check_custom_color1    gegl_color  (詳細下記)
[7] check_custom_color2    gegl_color
[8] show_help_button       int8
[9] use_cpu_accel          int8
[10] use_opencl            int8
[11] export_color_profile  int8
[12] export_comment        int8
[13] export_exif           int8
[14] export_xmp            int8
[15] export_iptc           int8
[16] update_metadata       int8
[17] default_display_id    int32
[18] app_name              string
[19] wm_class              string
[20] display_name          string
[21] monitor_number        int32
[22] timestamp             int32
[23] icon_theme_dir        string
[24] tile_cache_size       int64   (バイト単位)
[25] swap_path             string
[26] swap_compression      string
[27] num_processors        int32
```

##### gegl_color のサブフィールド

```
uint32  pixel_size       (バイト数。0 〜 40)
uint8[] pixel_data       (pixel_size バイト)
string  encoding         (BABL フォーマット名、例: "R'G'B'A u8")
uint32  icc_length       (ICC プロファイル長)
uint8[] icc_data         (icc_length バイト)
```

##### **危険な NULL 落とし穴**

- **`icc_length = 0` を送ると plugin がクラッシュ**: `_gimp_wire_read_gegl_color` は `icc_length=0` のとき `config->check_custom_iccN` を **NULL のまま**残す。続く `_gimp_config` で `g_bytes_get_data(NULL, ...)` が呼ばれ NULL deref → 0xC0000005。回避: ダミー 1 バイト (`0x00`) を ICC として送る。babl はパース失敗で `space=NULL` を返すが、後続処理は問題なし。
- **`swap_path = ""` (length=0 = NULL)**: `gimp_file_new_for_config_path(NULL, ...)` → `g_file_get_path()` で NULL deref。回避: `%TEMP%\cspbridge-gimp-swap` などの実在パス文字列を送る。
- **`pixel_size = 0` でも pixel GBytes は NULL**: ただし `_gimp_config` は色フォーマットチェックで bpp 不一致を検知して安全な代替値で `gegl_color_set_pixel` を呼ぶため、ICC ほど致命的ではない。とはいえ警告ログを避けるため有効な 4 バイト RGBA を送るのが望ましい。

これらの NULL 危険性は `_gimp_config` のソース上の前提（GIMP 本体 host が常に有効値を送ってくる）に由来する。host emulator は GIMP 本体の挙動を完全に模倣する必要がある。

#### GP_PROC_RUN ペイロードフォーマット

```
string  proc_name
uint32  n_params
GPParam[n_params]
```

各 `GPParam` のフォーマット（`libgimpbase/gimpprotocol.c` `_gp_params_write`）:

```
uint32  param_type                (GPParamType enum)
string  param->type_name          (outer: GValue の GType 名、g_type_name(type) の結果)
<param_type 依存データ>
```

#### filter 呼び出し時の各パラメーター

| param | param_type | outer type_name | データ |
|---|---|---|---|
| param[0]: run_mode | `GP_PARAM_TYPE_INT (=0)` | `"GimpRunMode"` | `int32 = 1` (GIMP_RUN_NONINTERACTIVE) |
| param[1]: image_id | `GP_PARAM_TYPE_INT (=0)` | `"GimpImage"` | `int32 = <image_id>` |
| param[2]: drawables | `GP_PARAM_TYPE_ID_ARRAY (=11)` | `"GimpCoreObjectArray"` | (下記参照) |

**param[2] drawables の IdArray ペイロード** (`_gp_params_write` IdArray ブランチ):

```
string  d_id_array.type_name     = "GimpItem"
        ※ GimpDrawable は GimpItem のサブタイプ。gimp_value_to_gp_param では
           GIMP_IS_ITEM() チェックで element_type = GIMP_TYPE_ITEM が決まり、
           g_type_name(GIMP_TYPE_ITEM) = "GimpItem" になる。"GimpDrawable" は誤り。
uint32  d_id_array.size          = 要素数
int32[] d_id_array.data[size]    = drawable ID 配列
```

**バグ注意**: GIMP 2.x ではドロアブルを単純な `GP_PARAM_TYPE_INT` で渡していた。GIMP 3 では `GP_PARAM_TYPE_ID_ARRAY` に変更されており、outer type_name も `"GimpObjectArray"` ではなく `"GimpCoreObjectArray"` が正しい。

#### GimpChoice パラメーターの wire 送信（2026-05-06 実機確認済み）

GimpChoice 型（`gimp_procedure_add_choice_argument()` で登録）のパラメーターを GP_PROC_RUN で送る際は：

- `param_type` = `GP_PARAM_TYPE_STRING` (= 2)
- `type_name` = `"gchararray"` ← **`"GimpChoice"` は誤り**
- `d_string` = choice の nick 文字列（例: `"adaptive"`, `"horizontal"`）

**根拠:** `GimpParamSpecChoice` は `GParamSpecString` のサブクラスで `klass->value_type = G_TYPE_STRING`。
受信側 `_gimp_gp_params_to_value_array()` が `g_type_from_name(type_name)` で型解決するため、
`"gchararray"`（`g_type_name(G_TYPE_STRING)` の値）でなければ
「unsupported deserialization to GValue of type 'GimpChoice'」エラーになる。
出典: `libgimp/gimpgpparams-body.c`, `libgimpbase/gimpchoice.c:454`

---

## 9. Wire Protocol / PDB スタブ仕様（`src/host/`）

フィルタープラグイン実行に必要な最小スタブ:

| プロシージャ | 実装内容 |
|---|---|
| `gimp_image_list()` | ダミー `image_id = 1` を返す |
| `gimp_drawable_get(image_id)` | 幅・高さ・タイプ（RGBA）を CSP バッファから取得して返す |
| `gimp_display_list()` | 空リストを返す |
| `gimp_tile_get(drawable_id, tile_num)` | CSP バッファの該当タイル領域を RGBA でコピーして返す |
| `gimp_tile_put(drawable_id, tile_num, data)` | 受信 RGBA データを CSP バッファの該当領域に書き戻す |
| UI 系コール | `GIMP_RUN_NONINTERACTIVE` 指定でスタブ不要（no-op） |

---

## 10. タイル転送仕様（`src/host/tile_transfer.cpp`）

| 項目 | 値 |
|---|---|
| タイルサイズ | 64×64 px（端タイルは余白分のみ） |
| ピクセルフォーマット | RGBA 8bit（4 bytes/px） |
| タイル単位データ量 | 64×64×4 = 16,384 bytes |
| タイルインデックス計算 | `tile_num = (y / 64) * tiles_per_row + (x / 64)` |

**スレッド安全設計**

- タイルバッファはスレッドローカルに確保（`thread_local std::array<uint8_t, 16384>`）
- CSP バッファからのタイル読み取りは `shared_mutex` の `shared_lock` で保護
- タイル書き戻しは `unique_lock` で排他制御

### 10.1 Wire Protocol メッセージシーケンス（2026-04-29 上流ソース確認済み）

確認ソース: `libgimpbase/gimpprotocol.c`（`_gp_tile_req_read/write`, `_gp_tile_ack_read/write`, `_gp_tile_data_read/write`）, `libgimp/gimptilebackendplugin.c`, `app/plug-in/gimpplugin-message.c`。

#### gimp_tile_get（プラグインがタイルを読む）

```
plugin → host : GP_TILE_REQ   { drawable_id(int32), tile_num(uint32), shadow(uint32) }
host   → plugin: GP_TILE_DATA  { drawable_id, tile_num, shadow, bpp, width, height,
                                  use_shm(uint32=0), pixel_data[width*height*bpp] }
plugin → host : GP_TILE_ACK   { (ペイロードなし) }
```

#### gimp_tile_put（プラグインがタイルを書き戻す）

```
plugin → host : GP_TILE_REQ   { drawable_id = -1, tile_num = 0, shadow = 0 }
                                ↑ 全フィールド固定値（実情報は次の DATA に乗る）
host   → plugin: GP_TILE_DATA  { -1, 0, 0, bpp=0, width=0, height=0, use_shm=0 }
                                ↑ **すべて 0 を送る**（length=0*0*0=0 で pixel_data 不要）
plugin → host : GP_TILE_DATA  { drawable_id, tile_num, shadow, bpp, width, height,
                                  use_shm=0, pixel_data[width*height*bpp] }
host   → plugin: GP_TILE_ACK   { (ペイロードなし) }
```

**重要 1**: `drawable_id == -1` が PUT のシグナル。`gimptilebackendplugin.c::gimp_tile_put` (line 413) は REQ で `drawable_id=-1, tile_num=0, shadow=0` を固定送信し、実タイル番号は次の DATA に乗せる。
**重要 2**: ホストのプロンプトは `bpp=0, width=0, height=0` にすること。受信側 `_gp_tile_data_read` (`libgimpbase/gimpprotocol.c:850`) は `use_shm==0` の時 `width*height*bpp` バイトの pixel_data を必ず読むため、非ゼロにするとプラグインが pixel_data 待ちでブロックする（GIMP 本体: `app/plug-in/gimpplugin-message.c:199-208`）。

### 10.2 メッセージペイロードフォーマット（上流確認済み）

**GP_TILE_REQ**（`libgimpbase/gimpprotocol.c` `_gp_tile_req_read`）:
```
uint32  drawable_id   (wire上は uint32; -1 は put トリガー)
uint32  tile_num
uint32  shadow        (0=通常バッファ, 1=シャドウバッファ)
```

**GP_TILE_ACK**: ペイロードなし（`_gp_tile_ack_read/write` は空スタブ）。

**GP_TILE_DATA**（`libgimpbase/gimpprotocol.c` `_gp_tile_data_read`）:
```
uint32  drawable_id
uint32  tile_num
uint32  shadow
uint32  bpp             (bytes per pixel; RGBA = 4)
uint32  width
uint32  height
uint32  use_shm         (0=後続にピクセルデータあり, 1=共有メモリ使用; PoC は常に 0)
uint8[] pixel_data      (use_shm==0 の場合のみ存在; length = width * height * bpp)
```

### 10.3 ホスト実装上の注意（tile_put ハンドシェイク）

`gimp_plug_in_handle_tile_put()` のシーケンスは次のとおり:
1. ホストが `GP_TILE_DATA`（drawable_id=-1, use_shm=0 のプロンプト）を送る
2. ホストがプラグインの `GP_TILE_DATA`（実際のピクセルデータ）を読む
3. ホストが `GP_TILE_ACK` を送る

`tile_transfer.cpp` の実装はこの順序を厳密に守ること。

**転送フロー**

```
CSP バッファ
  → RGBA 変換（buffer.cpp）
  → タイル分割（tile_transfer.cpp）
  → Wire Protocol 送信（wire_io.cpp）
  → GIMP プラグイン処理
  → Wire Protocol 受信（wire_io.cpp）
  → タイル結合（tile_transfer.cpp）
  → CSP バッファ形式に変換（buffer.cpp）
  → CSP レイヤーに書き戻し
```

### 10.4 GP_PARAM_TYPE_CURVE (= 15) ワイヤーフォーマット（2026-04-29 確認済み）

確認ソース: `libgimpbase/gimpprotocol.c` `_gp_params_read` / `_gp_params_write`, case `GP_PARAM_TYPE_CURVE`。

これは**可変長型**。レイアウト:

```
uint32   curve_type                          (GimpCurveType enum)
uint32   n_points
uint32   n_samples
double[] points      [2 * n_points 要素]     (各 double = 8 bytes big-endian、長さプレフィックスなし)
uint32[] point_types [n_points 要素]
double[] samples     [n_samples 要素]
```

読み飛ばす場合の C++ 擬似コード（`ReadExact` / `ReadUint32` レベル）:

```cpp
uint32_t curveType = ReadUint32(fd);
uint32_t nPoints   = ReadUint32(fd);
uint32_t nSamples  = ReadUint32(fd);
SkipBytes(fd, 2 * nPoints  * 8);   // points[]     — double × 2n
SkipBytes(fd, nPoints      * 4);   // point_types[] — uint32 × n
SkipBytes(fd, nSamples     * 8);   // samples[]    — double × n
```

double のワイヤー表現: 8 bytes を big-endian で書き込む（リトルエンディアンホストではバイトスワップ）。`libgimpbase/gimpwire.c` `_gimp_wire_read_double` / `_gimp_wire_write_double` で確認済み。

---

## 11. CSP プラグインエントリ仕様（`src/csp/`）

CSP フィルタープラグイン API のエクスポート関数を実装する。

**実行フロー**

1. CSP から選択範囲・アクティブレイヤーバッファを取得
2. CSP バッファ → RGBA に変換（`buffer.cpp`）
3. ホストエミュレーターを初期化（image_id, drawable_id を割り当て）
4. `PluginSession` を生成して GIMP プラグイン EXE をパイプ付きで起動（`wire_io.cpp`）
5. `GIMP_RUN_NONINTERACTIVE` でプラグインを呼び出す
6. Wire Protocol でタイル送受信（`wire_io.cpp` + `tile_transfer.cpp`）
7. 処理済み RGBA → CSP バッファ形式に変換
8. CSP レイヤーに書き戻し

### 11.1 CSP バッファ ↔ RGBA 変換仕様（`src/csp/buffer.cpp`）

確認ソース: `CSPBridgeSolidFill/CSP_FilterPlugIn/TriglavPlugInSDK/TriglavPlugInService.h`（オフスクリーン API）、`CSPBridgeEffects/Effects/PixelBuffer.cs`（C# リファレンス実装）。

#### CSP offscreen アクセスの特性

1. **Block-based**: offscreen はタイル状ブロックで管理。`getBlockRectCountProc` → `getBlockRectProc` → `getBlockImageProc` / `getBlockAlphaProc` の順で反復する。
2. **動的チャンネルインデックス**: RGB チャンネルの順番は不定。`getRGBChannelIndexProc` で `rIdx, gIdx, bIdx` を取得してから読み書きする。
3. **アルファの二択**: 分離バッファ（`getBlockAlphaProc` → `alphaPtr != null`）か、`pixBytes == 4` のとき非 RGB チャンネルとして画像データに埋め込まれている。

#### C++ API 設計（SDK headers: `TriglavPlugInSDK/`）

```cpp
// CSP ブロック形式を保持する中間構造体（CSP API 不要でテスト可）
struct CspBuffer {
    uint32_t              width, height;
    int32_t               rIdx, gIdx, bIdx; // getRGBChannelIndexProc で取得
    int32_t               pixBytes;         // 画像 1px のバイト数（通常 3 or 4）
    int32_t               aPixBytes;        // アルファ 1px のバイト数（0 = 分離なし）
    std::vector<uint8_t>  imageData;        // width * height * pixBytes、row-major
    std::vector<uint8_t>  alphaData;        // width * height * aPixBytes（分離アルファあり時）
};

// CSP offscreen → CspBuffer（getBlockImageProc / getBlockAlphaProc）
CspBuffer ReadFromOffscreen(
    const TriglavPlugInOffscreenService* svc,
    TriglavPlugInOffscreenObject         offscreen,
    const TriglavPlugInRect&             selectRect);

// CspBuffer → CSP offscreen 書き戻し
void WriteToOffscreen(
    const TriglavPlugInOffscreenService* svc,
    TriglavPlugInOffscreenObject         offscreen,
    const TriglavPlugInRect&             selectRect,
    const CspBuffer&                     buf);

// チャンネル並べ替え: CspBuffer → flat RGBA (R=0,G=1,B=2,A=3)
std::vector<uint8_t> CspToRgba(const CspBuffer& buf);

// flat RGBA → CspBuffer（チャンネル順・pixBytes は layout コピー）
CspBuffer RgbaToCsp(
    const uint8_t*    rgba,
    uint32_t          width,
    uint32_t          height,
    const CspBuffer&  layout);
```

#### RGBA 専用制約（PoC スコープ）

`ReadFromOffscreen` / `WriteToOffscreen` は `getChannelOrderProc` で取得したチャンネルオーダーが `kTriglavPlugInOffscreenChannelOrderRGBAlpha` でない場合に `std::runtime_error` を投げる。

**理由**: GIMP Wire Protocol の tile フォーマットは RGBA 8bpc 固定（bpp=4）。CMYK・グレースケールに対応する tile モードは Wire Protocol に存在しない。加えて `tile_transfer.cpp` は bpp=4 を前提としているため、他のカラーモードへの拡張には Wire Protocol 改造と ICC カラー変換が必要であり、PoC スコープ外。

**ユーザー向け対処**: CSP 上でレイヤーを RGBA カラーモードに変換してから本プラグインを実行すること。

#### GIMP 側 RGBA フォーマット

8bit flat、R=0 G=1 B=2 A=3 固定順、stride = width × 4。`tile_transfer.cpp` がこのフォーマットを直接扱う。

#### SDK ヘッダーパス

`extern/TriglavPlugInSDK/` にコピー済み（2026-04-29）。元ファイルは `D:\Develop\Projects\CSPBridgeSolidFill\CSP_FilterPlugIn\FilterPlugIn\TriglavPlugInSDK\`。

コピーしたヘッダー:
- `TriglavPlugInDefine.h` — 定数・マクロ定義（チャンネルオーダー等）
- `TriglavPlugInType.h` — 基本型（`TriglavPlugInRect`, `TriglavPlugInPoint` 等）
- `TriglavPlugInExtern.h` — `TRIGLAV_PLUGIN_API`, `TRIGLAV_PLUGIN_PACK` 等
- `TriglavPlugInService.h` — `TriglavPlugInOffscreenService` 構造体（最重要）
- `TriglavPlugInRecord.h` — フィルター初期化・実行レコード
- `TriglavPlugInRecordFunction.h` — レコードアクセスマクロ
- `TriglavPlugInServer.h` — `TriglavPlugInServer` 構造体
- `TriglavPlugInMain.h` — `TriglavPluginCall` エントリ宣言
- `TriglavPlugInSDK.h` — 上記の統合ヘッダー

`meson.build` に `include_directories('extern/TriglavPlugInSDK')` を追加済み（`core_lib` + `test_exe` 両方）。

#### SDK API の確認済み事実（2026-04-29、ヘッダー調査）

| SDK API | 実際のシンボル | 備考 |
|---|---|---|
| RGB チャンネルインデックス取得 | `svc->getRGBChannelIndexProc(&rIdx, &gIdx, &bIdx, offscreen)` | 戻り値 0 = Success |
| チャンネルオーダー取得 | `svc->getChannelOrderProc(&order, offscreen)` | `kTriglavPlugInOffscreenChannelOrderRGBAlpha = 0x03` |
| ブロック数取得 | `svc->getBlockRectCountProc(&count, offscreen, &bounds)` | |
| ブロック矩形取得 | `svc->getBlockRectProc(&rect, index, offscreen, &bounds)` | |
| 画像ブロック取得 | `svc->getBlockImageProc(&ptr, &rowBytes, &pixBytes, &rect, offscreen, &pos)` | ptr == nullptr はスキップ |
| アルファブロック取得 | `svc->getBlockAlphaProc(&ptr, &rowBytes, &pixBytes, &rect, offscreen, &pos)` | ptr == nullptr = 分離アルファなし |
| API 成功判定 | `kTriglavPlugInAPIResultSuccess = 0` | `kTriglavPlugInAPIResultFailed = -1` |

**RGBA レイヤーのアルファ取得ロジック**（C# `PixelBuffer.cs` から確認）:
1. `getBlockAlphaProc` が `alphaPtr != nullptr` → 分離アルファ（先頭 `aPixBytes` バイト）
2. `alphaPtr == nullptr && pixBytes == 4` → RGB 以外の第 4 チャンネルインデックスを `effectiveAlphaIdx` として使用
3. それ以外 → 0xFF 固定（不透明）

**実機確認済み事項（2026-05-06）**:
- ✓ 通常の RGBA レイヤーで `rIdx=0, gIdx=1, bIdx=2, effectiveAlphaIdx=3` — checkerboard/despeckle/blinds の E2E 動作で確認済み
- ✓ `WriteToOffscreen` 後に `updateDestinationOffscreenRectProc` を呼ぶ — `plugin_entry.cpp` の `HandleFilterRun` で実装済み、実機正常動作確認済み

#### 実装ファイル（2026-04-29 完了）

- `src/csp/buffer.h` — `CspBuffer` 構造体定義、公開 API 宣言（Doxygen 付き）
- `src/csp/buffer.cpp` — `ReadFromOffscreen`, `WriteToOffscreen`, `CspToRgba`, `RgbaToCsp` 実装
- `tests/test_buffer.cpp` — 6 テストケース（Catch2）。`CspToRgba` / `RgbaToCsp` のみを検証（CSP API 不要）

テスト実行: `meson test -C builddir` → All tests passed (1366 assertions in 57 test cases)

### 11.2 plugin_entry 実装仕様（`src/csp/plugin_entry.cpp`）（2026-04-30 完了）

#### SDK 実装上の確認済み事実

| 事実 | 詳細 |
|---|---|
| `TriglavPlugInServer` フィールドアクセス | `pluginServer->recordSuite`（値）、`pluginServer->serviceSuite`（値）、`pluginServer->hostObject`（ポインタ）。マクロは `&recSuite` を渡す |
| レコードマクロの引数 | `TriglavPlugInFilterInitializeSetFilterCategoryName(&recSuite, host, strObj, accessKey)` — 第 4 引数 accessKey は `'\0'` で可 |
| `TriglavPlugInStringService::createWithAsciiStringProc` | `(out*, ascii, length)` の length は NUL を除いた文字数 |
| `TriglavPlugInExtern.h` のプラットフォーム検出 | `#if defined(_WINDOWS)` を使用。MSVC は `_WIN32` を自動定義するが `_WINDOWS` は定義しない。`plugin_entry.cpp` の先頭で `#ifndef _WINDOWS` / `#define _WINDOWS` が必要 |
| `WIN32_LEAN_AND_MEAN` / `NOMINMAX` | meson.build が `/D` で設定済みのため、`.cpp` 内では `#ifndef` ガード付きで定義する |
| `core_lib` の include パス | `extern/TriglavPlugInSDK` のみ追加。`src/` は追加されない。`plugin_entry.cpp` は `"../config/config.h"` など `src/csp/` 相対パスでインクルードする |
| `GIMP_PLUGIN_ID` の扱い | `core_lib` コンパイル時は未定義のためフォールバック `""` を使用。実際の DLL ターゲットでは meson.build から `/DGIMP_PLUGIN_ID="<id>"` が渡される |
| Windows での DLL 自身ディレクトリ取得 | `DllMain` で `g_hModule` に `HMODULE` を保存し `GetModuleFileNameW` で取得。`DisableThreadLibraryCalls` も同時に呼ぶ |
| Mac での dylib 自身ディレクトリ取得 | `dladdr(reinterpret_cast<void*>(&TriglavPluginCall), &info)` で `info.dli_fname` からディレクトリを取得 |
| `HandleFilterRun` 書き戻し通知 | `runRec->updateDestinationOffscreenRectProc(hostObject, &selectRect)` を呼ぶ（2026-05-06 実機確認済み） |

#### plugin_entry.cpp のファイル構成

```
src/csp/plugin_entry.h   — GetModuleDir() 前方宣言のみ。他モジュールからのインクルード不要
src/csp/plugin_entry.cpp — TriglavPluginCall + DllMain(Win) / dladdr(Mac) + 3 ハンドラー関数
```

ハンドラー関数はすべて無名 namespace 内に実装（DLL エクスポートは `TriglavPluginCall` のみ）。
`ScopedTriglavString` RAII クラスで文字列オブジェクトのリリース漏れを防ぐ。

---

## 12. ライセンス対応

| モジュール | ライセンス | 取り込み方法（PoC） | 対応 |
|---|---|---|---|
| `libgimpwire.dll/dylib` | LGPL v3 | system インストール済み GIMP 3 を pkg-config 経由で動的リンク | 無改変で動的リンク・同梱。差分なし |
| `libgimpbase.dll/dylib` | LGPL v3 | 同上 | 同上 |
| `MyGimpHost.dll/dylib` | 独自（非公開） | リポジトリ内 `src/` で独自実装 | 完全独自実装。PoC だが原則商用販売も可 |
| nlohmann/json | MIT | `subprojects/nlohmann_json.wrap`（WrapDB） | header-only。制限なし |
| Catch2 | BSL-1.0 | `subprojects/catch2.wrap`（WrapDB） | テスト用のみ |
| Boost | BSL-1.0 | system インストール（MSYS2 / vcpkg / Homebrew 等）、`dependency('boost', required: false)` | Boost.Process 等。制限なし |

- LGPL v3 要件として動的リンクを維持する（GIMP ライブラリはビルドに含めず、OS 側 GIMP 3 インストールから DLL/dylib を引く形）
- 本プロジェクト自身が `libgimpwire` / `libgimpbase` を改変した場合のみ、その差分の公開義務あり（PoC では無改変のため不要）
- 将来 `subprojects/gimp.wrap` を有効化して GIMP ソースを取り込む場合も、**libgimpwire / libgimpbase のみを static_library ではなく shared_library としてビルドする**ことで LGPL v3 要件を維持する。`libgimpbase/*.c` / `libgimpwire/*.c` 以外（`app/`, `gimp/` 以下の GPL v3 部分）は一切ビルドしない

---

## 13. 実装優先順位（プロトタイプ）

**フェーズ 0: スキャナーツール ✓ 完了**

1. ✓ `tools/scanner/scan_and_select.py` — Wire Protocol クエリフェーズ（`-query` argv + `GP_PROC_INSTALL` 受信 + `GP_PROC_RUN` PDB コールバック傍受・`GP_PROC_RETURN` 応答）。**GIMP 3.0 には GP_QUERY メッセージは存在しない**（§5.3 参照）
2. ✓ `tools/scanner/scan_and_select.py` — tkinter チェックリスト GUI
3. ✓ `tools/scanner/scan_and_select.py` — plugins.json 出力
4. ✓ `scripts/list_plugin_ids.py` — meson 用 id 一覧出力スクリプト

**フェーズ 1: ブリッジ本体 ✓ 完了（2026-05-06 実機 E2E 検証済み）**

5. ✓ `config/config` — JSON 設定ファイル読み込み・プラグイン EXE パス解決（`plugins.json` 対応含む）
6. ✓ `ipc/process` — 子プロセス起動＋パイプ接続
7. ✓ `ipc/wire_io` — `PluginSession` + `std::jthread` によるマルチスレッド通信基盤
8. ✓ `host/pdb_stubs` — 最小スタブ（上記5種）、`shared_mutex` で保護
9. ✓ `host/tile_transfer` — gimp_tile_get / gimp_tile_put（スレッドローカルバッファ）
10. ✓ `csp/buffer` — CSP ↔ RGBA 変換
11. ✓ `csp/plugin_entry` — CSP プラグインとして統合・複数セッション並列実行

**フェーズ 2: 追加プラグイン実装（進行中）**

- checkerboard / despeckle / blinds の 3 本が動作確認済み（2026-05-06）
- 残り 37 本（docs/gimp3_plugin_availability.md 参照）から順次追加

---

## 14. テスト仕様

詳細は [`docs/spec_test.md`](spec_test.md) を参照。

テストフレームワーク: **Catch2**（`subprojects/catch2.wrap`）
実行コマンド: `meson test` / `meson test -v`

---

## 15. プロジェクト運用ルール

本仕様書の技術内容と並行して、以下のプロジェクト運用ルールを定める。詳細は各メモリファイル・エージェント定義ファイルに保持されているが、仕様書にも記録する。

### 15.1 発見は都度 spec.md に反映する

実装・調査中に本書の記述と現実が食い違うことが判明したら、**発見のたびに** 該当節を書き換えてから作業を続行する。バッチで後回しにしない。

実例：Phase 0 スキャナー実装中に上流 GIMP 3.0 ソースを読んで「`GP_QUERY` メッセージは GIMP 3.0 には存在しない」ことが判明 → §5.3 を即座に書き換え、GIMP 2.x 由来の誤情報を排除した。

### 15.2 エージェントは得た知見を二重に反映する

`.claude/agents/` 配下のサブエージェント（`project-leader`, `gimp-protocol-engineer`, `cpp-systems-engineer`, `csp-plugin-engineer`）が作業中に得た「このプロジェクト固有で、次セッションでも再利用すべき具体的知見」は、**以下の両方**に書く：

1. **自身のエージェント定義ファイル** `.claude/agents/<name>.md` — 次回同じエージェントが呼ばれた時にすぐ参照できる事実
2. **本仕様書 `docs/spec.md`** — プロジェクト全体の恒久記録（§該当節に追記）

対象になる知見の例：
- Upstream ソース（`gitlab.gnome.org/GNOME/gimp` GIMP_3_0_0 タグ）を読んで確定した enum 値 / バイトレイアウト / 関数シグネチャ
- 実機検証で判明した OS 固有挙動（例: Windows MSVCRT fd 継承、Mac dylib RPATH）
- 設計判断と却下理由（例: GIMP wrap を packagefiles/ placeholder に留めた理由）
- ビルド・テスト実行で見つかった互換性制約

汎用的な C++ / Python / Meson の常識は書かない。**プロジェクト固有かつ再利用価値のある事実のみ**。両方に書いた場合は相互参照する（例: "spec.md §5.6 参照"）。

### 15.3 スキル・フック・メモリ

プロジェクトローカルの自動化は以下に集約：

| 機構 | 場所 | 役割 |
|---|---|---|
| スキル `/phase-step` | `.claude/skills/phase-step/SKILL.md` | コンポーネント実装を delegate → PL review → test → commit/push まで通す |
| スキル `/gimp-ref` | `.claude/skills/gimp-ref/SKILL.md` | 上流 GIMP ソースを参照して仕様を確定 |
| フック `format-cpp.sh` | `.claude/hooks/` | Edit/Write 後に clang-format -i（best-effort） |
| フック `check-python.sh` | `.claude/hooks/` | Edit/Write 後に py_compile |
| フック `block-local-settings-commit.sh` | `.claude/hooks/` | `.claude/settings.local.json` の意図しない commit を阻止 |
| 共有設定 | `.claude/settings.json` | 上記フック配線 |
| 個人権限キャッシュ | `.claude/settings.local.json`（gitignore 済） | 各開発者ローカル |

---

## 16. 参照ドキュメント

- GIMP 3.0 API リファレンス: `developer.gimp.org/resource/api/`
- Wire Protocol / プラグインアーキテクチャ: `developer.gimp.org/resource/about-plugins/`
- プラグイン実装チュートリアル（C サンプルあり）: `developer.gimp.org/resource/writing-a-plug-in/`
- 技術調査・ライセンス確認: `docs/GIMP_Bridge_Summary.md`

---

## 17. インストール済み GIMP プラグイン一覧（2026-04-29 確認）

**環境**: GIMP 3.x、`%LOCALAPPDATA%\Programs\GIMP 3\lib\gimp\3.0\plug-ins\`

スキャナー（`tools/scanner/scan_and_select.py`）で bridge 利用可能かを事前評価する際の参照リスト。
EXE が存在しないディレクトリはスクリプト系（Script-Fu / Python）で Wire Protocol を使わない可能性あり。

### 17.1 フィルター系（bridge 利用が有望なもの）

| プラグイン名 | EXE | 実装済み | 概要 |
|---|---|---|---|
| `blinds` | ✓ | ✓ | ブラインド（百葉窓）効果 |
| `border-average` | ✓ | | 境界色の平均化 |
| `checkerboard` | ✓ | ✓ | チェッカーボードパターン生成 |
| `cml-explorer` | ✓ | | セルオートマトン |
| `contrast-retinex` | ✓ | | Retinex コントラスト強調 |
| `curve-bend` | ✓ | | カーブに沿った変形 |
| `depth-merge` | ✓ | | 深度マップを使った合成 |
| `despeckle` | ✓ | ✓ | スペックル（粒状ノイズ）除去 |
| `destripe` | ✓ | | スキャンライン縞除去 |
| `film` | ✓ | | フィルムストリップ合成 |
| `flame` | ✓ | | フラクタル炎 |
| `fractal-explorer` | ✓ | | フラクタル探索 |
| `gfig` | ✓ | | ベクター幾何図形描画 |
| `gimpressionist` | ✓ | | 絵画調フィルター |
| `gradient-flare` | ✓ | | グラデーションフレア |
| `gradient-map` | ✓ | | グラデーションマッピング |
| `guillotine` | ✓ | | ガイドラインで画像分割 |
| `hot` | ✓ | | NTSC/PAL 安全色域補正 |
| `ifs-compose` | ✓ | | IFS フラクタル合成 |
| `jigsaw` | ✓ | | ジグソーパズル効果 |
| `lighting` | ✓ | | 3D ライティングエフェクト |
| `map-object` | ✓ | | 3D オブジェクトへのマッピング |
| `nl-filter` | ✓ | | 非線形フィルター（強化・エッジ等） |
| `pagecurl` | ✓ | | ページめくり効果 |
| `qbist` | ✓ | | ランダムテクスチャ生成 |
| `sample-colorize` | ✓ | | サンプル色彩化 |
| `smooth-palette` | ✓ | | パレットスムージング |
| `sparkle` | ✓ | | きらめきエフェクト |
| `sphere-designer` | ✓ | | 球面デザイン |
| `tile` | ✓ | | タイル分割 |
| `tile-small` | ✓ | | 小タイル効果 |
| `van-gogh-lic` | ✓ | | LIC（線積分合成）van Gogh 風 |
| `warp` | ✓ | | ワープ変形 |
| `wavelet-decompose` | ✓ | | ウェーブレット分解 |

### 17.2 UI 付き / 選択操作系

| プラグイン名 | EXE | 概要 |
|---|---|---|
| `align-layers` | ✓ | レイヤー整列（GIMP_RUN_NONINTERACTIVE 対応要確認） |
| `animation-optimize` | ✓ | アニメーション最適化 |
| `animation-play` | ✓ | アニメーション再生（UI 必須、bridge 対象外） |
| `colormap-remap` | ✓ | カラーマップ並べ替え |
| `compose` | ✓ | チャンネル合成 |
| `crop-zealous` | ✓ | Zealous Crop（自動クロップ） |
| `decompose` | ✓ | チャンネル分解 |
| `imagemap` | ✓ | HTML イメージマップ（UI 必須、bridge 対象外） |
| `selection-to-path` | ✓ | 選択範囲をパスに変換 |

### 17.3 ファイル入出力系（bridge 対象外）

Wire Protocol の run フェーズで `GIMP_RUN_NONINTERACTIVE` を渡せば起動はできるが、
ファイル操作が目的のため CSP bridge の用途には合わない。

`file-bmp`, `file-cel`, `file-dds`, `file-dicom`, `file-exr`, `file-farbfeld`, `file-fits`,
`file-fli`, `file-gbr`, `file-gegl`, `file-gif-export`, `file-gif-load`, `file-gih`,
`file-glob`, `file-header`, `file-heif`, `file-html-table`, `file-icns`, `file-ico`,
`file-iff`, `file-jp2`, `file-jpeg`, `file-jpegxl`, `file-lnk`, `file-mng`, `file-paa`,
`file-pat`, `file-pcx`, `file-pdf-export`, `file-pdf-load`, `file-pix`, `file-png`,
`file-pnm`, `file-ps`, `file-psd`, `file-psp`, `file-pvr`, `file-qoi`, `file-raw-data`,
`file-raw-placeholder`, `file-rawtherapee`, `file-another-rawtherapee`, `file-seattle-filmworks`,
`file-sgi`, `file-sunras`, `file-svg`, `file-tga`, `file-tiff`, `file-tim`, `file-wbmp`,
`file-webp`, `file-wmf`, `file-xbm`, `file-xpm`, `file-xwd`, `file-compressor`, `file-csource`,
`file-darktable`, `file-aa`

### 17.4 スクリプト系・管理系（EXE なし / bridge 対象外）

EXE が存在せず、Script-Fu / Python / GEGL ベースで動作するもの:

`colorxhtml`, `file-openraster`, `foggify`, `gradients-save-as-css`, `histogram-export`,
`palette-export-as-kpl`, `palette-offset`, `palette-sort`, `palette-to-gradient`,
`python-console`, `python-eval`, `spyro-plus`, `test-sphere-v3`

### 17.5 システム系（bridge 対象外）

`busy-dialog`, `filter-browser`, `help`, `metadata-editor`, `metadata-viewer`,
`plugin-browser`, `print`, `procedure-browser`, `screenshot`, `script-fu`,
`script-fu-server`, `unit-editor`, `web-browser`, `wia`

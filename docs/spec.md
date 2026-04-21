# CSPBridgeGimp 実装仕様書

## 1. 概要

> **本プロジェクトは技術検証（PoC）を目的としたものであり、有料コンテンツ・商用製品ではありません。**

Clip Studio Paint (CSP) プラグインから GIMP フィルタープラグインをブリッジ実行するホストエミュレーターの実装仕様。CSP プラグインが GIMP ホストを模倣し、GIMP プラグイン EXE を子プロセスとして起動、Wire Protocol でタイル送受信を行い、処理結果を CSP レイヤーに書き戻す。

---

## 2. モジュール構成

```
CSPBridgeGimp/
├── meson.build
├── config/
│   └── bridge_config.json             # プラグインインストール先などの設定ファイル
├── src/
│   ├── host/
│   │   ├── host.cpp / host.h           # エントリポイント・初期化
│   │   ├── pdb_stubs.cpp / pdb_stubs.h # PDB プロシージャスタブ群
│   │   └── tile_transfer.cpp / tile_transfer.h  # gimp_tile_get/put 実装
│   ├── ipc/
│   │   ├── process.cpp / process.h     # GIMP プラグイン EXE 起動
│   │   └── wire_io.cpp / wire_io.h     # libgimpwire ラッパー・PluginSession
│   ├── config/
│   │   └── config.cpp / config.h       # JSON 設定ファイル読み込み
│   └── csp/
│       ├── plugin_entry.cpp / plugin_entry.h  # CSP エクスポート関数
│       └── buffer.cpp / buffer.h       # CSP ↔ RGBA 変換
├── scripts/
│   └── read_config.py                 # meson setup 時に JSON を読んでパスを展開するスクリプト
├── build/
│   └── cross/                         # クロスコンパイル用 cross-file テンプレート
├── tests/
│   ├── test_config.cpp
│   ├── test_tile_transfer.cpp
│   ├── test_buffer.cpp
│   ├── test_wire_io.cpp
│   └── test_concurrency.cpp
├── subprojects/
│   ├── gimp.wrap          # GIMP リポジトリの特定タグを参照（libgimpwire/libgimpbase を取得）
│   ├── nlohmann_json.wrap # nlohmann/json（JSON パーサー）
│   ├── catch2.wrap        # テストフレームワーク
│   ├── boost.wrap         # Boost（Boost.Process 等）
│   └── packagefiles/
│       └── gimp/          # wrap 用パッチ・meson.build オーバーライド（必要な場合のみ）
└── docs/
    ├── GIMP_Bridge_Summary.md
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

## 4. ビルドシステム（Meson）

- `libgimpwire` / `libgimpbase` は Meson wrap ファイル（`subprojects/gimp.wrap`）で GIMP リポジトリの特定タグを参照して取り込む
- `MyGimpHost` は `shared_library()` としてビルド（Windows: DLL、Mac: dylib）
- プラットフォーム分岐は `host_machine.system()` で判定（対象: Windows / Mac のみ）

**`subprojects/gimp.wrap`** の例:

```ini
[wrap-git]
url = https://gitlab.gnome.org/GNOME/gimp.git
revision = GIMP_3_0_0          # 使用するタグまたはコミットハッシュ
depth = 1

[provide]
libgimpwire = libgimpwire_dep
libgimpbase = libgimpbase_dep
```

- `meson subprojects download` で取得、`meson subprojects update` で更新
- GIMP 本体のビルドは不要。`libgimpwire` / `libgimpbase` のターゲットのみ使用する
- 無改変で使用するため LGPL v3 の差分公開義務なし

```meson
# meson.build（概略）
project('CSPBridgeGimp', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++23'])

# config/bridge_config.json から install 先を取得
py = import('python').find_installation('python3', required: true)
config_reader = files('scripts/read_config.py')
platform = host_machine.system()   # 'windows' or 'darwin'

csp_output_dir = run_command(
  py, config_reader,
  '--config', meson.source_root() / 'config/bridge_config.json',
  '--platform', platform,
  '--key', 'csp_plugin_output_dir',
  check: true,
).stdout().strip()

libgimpwire_dep = dependency('libgimpwire', fallback: ['gimp', 'libgimpwire_dep'])
libgimpbase_dep = dependency('libgimpbase', fallback: ['gimp', 'libgimpbase_dep'])
nlohmann_json_dep = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])
boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: true)

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

shared_library('MyGimpHost', srcs,
  dependencies: [libgimpwire_dep, libgimpbase_dep, nlohmann_json_dep, boost_dep],
  install: true,
  install_dir: csp_output_dir,
)
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

## 5. SIMD / Intrinsics 方針

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

## 6. IPC レイヤー仕様（`src/ipc/`）

### 6.1 子プロセス起動

**Boost.Process** を使用してプラットフォーム差異を吸収する。`CreateProcess`（Windows）/ `fork+exec`（Mac）の個別実装は不要。

```cpp
// process.cpp — Win/Mac 共通
#include <boost/process.hpp>
namespace bp = boost::process;

bp::child LaunchPlugin(const std::string& exePath,
                       bp::ipstream& outStream,
                       bp::opstream& inStream)
{
    return bp::child(
        exePath,
        bp::std_out > outStream,
        bp::std_in  < inStream
    );
}
```

- `bp::ipstream` / `bp::opstream` がパイプを抽象化し、`libgimpwire` の read/write に渡す
- 子プロセス終了検知: `child.wait()` または `child.running()` で監視

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

---

## 7. Wire Protocol / PDB スタブ仕様（`src/host/`）

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

## 8. タイル転送仕様（`src/host/tile_transfer.cpp`）

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

---

## 9. CSP プラグインエントリ仕様（`src/csp/`）

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

---

## 10. ライセンス対応

| モジュール | ライセンス | 対応 |
|---|---|---|
| `libgimpwire.dll/dylib` | LGPL v3 | 無改変で動的リンク・同梱。差分なし |
| `libgimpbase.dll/dylib` | LGPL v3 | 同上 |
| `MyGimpHost.dll/dylib` | 独自（非公開） | 完全独自実装。商用販売可 |
| nlohmann/json | MIT | header-only。制限なし |
| Boost | BSL-1.0 | Boost.Process 等。制限なし |

- スタティックリンク禁止（LGPL v3 要件）
- GIMP ライブラリを改変した場合はその差分のみ公開義務あり（無改変なら不要）

---

## 11. 実装優先順位（プロトタイプ）

1. `config/config` — JSON 設定ファイル読み込み・プラグイン EXE パス解決
2. `ipc/process` — 子プロセス起動＋パイプ接続
3. `ipc/wire_io` — `PluginSession` + `std::jthread` によるマルチスレッド通信基盤
4. `host/pdb_stubs` — 最小スタブ（上記5種）、`shared_mutex` で保護
5. `host/tile_transfer` — gimp_tile_get / gimp_tile_put（スレッドローカルバッファ）
6. `csp/buffer` — CSP ↔ RGBA 変換
7. `csp/plugin_entry` — CSP プラグインとして統合・複数セッション並列実行

---

## 12. テスト仕様

詳細は [`docs/spec_test.md`](spec_test.md) を参照。

テストフレームワーク: **Catch2**（`subprojects/catch2.wrap`）
実行コマンド: `meson test` / `meson test -v`

---

## 13. 参照ドキュメント

- GIMP 3.0 API リファレンス: `developer.gimp.org/resource/api/`
- Wire Protocol / プラグインアーキテクチャ: `developer.gimp.org/resource/about-plugins/`
- プラグイン実装チュートリアル（C サンプルあり）: `developer.gimp.org/resource/writing-a-plug-in/`
- 技術調査・ライセンス確認: `docs/GIMP_Bridge_Summary.md`

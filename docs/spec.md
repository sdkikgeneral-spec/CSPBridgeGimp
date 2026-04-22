# CSPBridgeGimp 実装仕様書

## 1. 概要

> **本プロジェクトは技術検証（PoC）を目的としたものであり、有料コンテンツ・商用製品ではありません。**

Clip Studio Paint (CSP) プラグインから GIMP フィルタープラグインをブリッジ実行するホストエミュレーターの実装仕様。CSP プラグインが GIMP ホストを模倣し、GIMP プラグイン EXE を子プロセスとして起動、Wire Protocol でタイル送受信を行い、処理結果を CSP レイヤーに書き戻す。

---

## 2. モジュール構成

```
CSPBridgeGimp/
├── meson.build
├── plugins.json                        # スキャナーツールが生成するプラグイン定義ファイル
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

### 4.1 基本方針

CSP は **1エフェクト = 1プラグイン DLL** の構造を持つ。GIMP プラグイン数は事前に確定できないため、**CSPBridge パターン**（`EFFECT_ID` のビルド時固定）を応用する。

```
plugins.json（スキャナー生成）
    ↓ meson setup 時に読み込み
meson.build
    ↓ foreach でプラグイン数だけ shared_library() を生成
CSPBridgeGimp_GaussBlur.dll    ← GIMP_PLUGIN_ID="GaussBlur" でビルド
CSPBridgeGimp_Unsharp.dll      ← GIMP_PLUGIN_ID="Unsharp" でビルド
CSPBridgeGimp_Pixelize.dll     ← GIMP_PLUGIN_ID="Pixelize" でビルド
```

DLL ソースは1本共通。`GIMP_PLUGIN_ID` マクロがビルド時に確定し、実行時に `plugins.json` から自分の設定を引く。

### 4.2 CSPBridge との対応関係

| CSPBridge | CSPBridgeGimp |
|---|---|
| `EFFECT_ID="Blur"` | `GIMP_PLUGIN_ID="GaussBlur"` |
| `CSPBridgeEffects.dll`（C# 共通処理） | `plugins.json`（共通メタデータ） |
| `CSPBridgeBlur.dll` / `CSPBridgeHSV.dll` | `CSPBridgeGimp_GaussBlur.dll` / `CSPBridgeGimp_Unsharp.dll` |

### 4.3 plugins.json スキーマ

スキャナーツール（`tools/scanner/scan_and_select.py`）が生成する。

```json
[
  {
    "id":        "GaussBlur",
    "exe":       "plug-in-gauss.exe",
    "procedure": "plug-in-gauss",
    "menu":      "Filters/Blur/Gaussian Blur",
    "blurb":     "Apply a gaussian blur",
    "params": [
      { "type": "INT32",    "name": "run-mode" },
      { "type": "IMAGE",    "name": "image" },
      { "type": "DRAWABLE", "name": "drawable" },
      { "type": "INT32",    "name": "horizontal" },
      { "type": "INT32",    "name": "vertical" },
      { "type": "INT32",    "name": "method" }
    ]
  }
]
```

| フィールド | 内容 |
|---|---|
| `id` | DLL 名サフィックス兼 `GIMP_PLUGIN_ID`（ASCII、スペースなし） |
| `exe` | GIMP プラグイン EXE のファイル名 |
| `procedure` | GIMP プロシージャ名（`plug-in-gauss` 等） |
| `menu` | CSP エフェクトメニューへの配置パス |
| `blurb` | 説明文 |
| `params` | パラメーター定義（`GP_PROC_INSTALL` から取得） |

`plugins.json` はリポジトリに含めてよい。再スキャンしたい場合のみスキャナーを再実行する。

### 4.4 DLL 実装（実行時の self-identification）

```cpp
// plugin_entry.cpp
const std::string pluginId{ GIMP_PLUGIN_ID };   // ビルド時に確定

// plugins.json から自分の設定を取得
auto config = FindPluginEntry(
    GetThisDllDirectory() + "/plugins.json", pluginId);
// → config.exe = "plug-in-gauss.exe"
// → config.params = [...]
```

---

## 5. プラグインスキャナーツール（`tools/scanner/scan_and_select.py`）

### 5.1 役割と位置づけ

`meson setup` の**前に手動で実行**する独立ツール。`plugins.json` が唯一の出力成果物であり、以降のビルドはこのファイルに依存する。

```
① scan_and_select.py を実行（手動・1回）
        ↓  plugins.json を保存
② meson setup builddir
③ meson compile -C builddir
④ meson install -C builddir
```

### 5.2 処理フロー

```
1. bridge_config.json から plugin_search_paths を読む
2. 各パスを走査して *.exe を列挙
3. 各 EXE に対してクエリフェーズを実行：
   a. 2 本の匿名パイプを作成（親 ↔ 子の双方向、継承可能 fd）
   b. 子プロセス起動:
      <exe> -gimp <protocol_version> <read_fd> <write_fd> -query 0
      * protocol_version は 10 進文字列（GIMP 3.0 = 277 / 0x0115）
      * read_fd / write_fd は子から見た fd 番号（親が作った fd をそのまま継承）
      * 4 番目の引数が "-query" のとき、プラグインは GP_CONFIG を
        待たずにいきなり GP_PROC_INSTALL を書き始める
      * 最後の 0 は GIMP_STACK_TRACE_NEVER
   c. 子からのメッセージを EOF までループ読み取り：
      - GP_PROC_INSTALL (=9): プロシージャ名 / 引数定義などを収集
      - GP_PROC_RUN (=5): プラグインからの PDB 呼び出し。
        menu_label / blurb 等は GIMP 3.0 では GP_PROC_INSTALL に
        含まれず、この PDB コールバックで送られてくる。
        スキャナーは空の成功 GP_PROC_RETURN を返して進行させる
      - GP_HAS_INIT (=12) / GP_PROC_UNINSTALL (=10): 読み飛ばし
      - その他は未知メッセージとして当該 EXE のスキャンを中止
   d. EOF（プラグイン自身が exit）または timeout で終了
4. tkinter チェックリストダイアログで一覧表示
5. ユーザーがチェックを入れたプラグインのみ plugins.json に書き出し
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

```meson
# meson.build（概略）
project('CSPBridgeGimp', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++23'])

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

**フェーズ 0: スキャナーツール（meson より先に完成させる）**

1. `tools/scanner/scan_and_select.py` — Wire Protocol クエリフェーズ（`-query` argv + `GP_PROC_INSTALL` 受信 + `GP_PROC_RUN` PDB コールバック傍受・`GP_PROC_RETURN` 応答）。**GIMP 3.0 には GP_QUERY メッセージは存在しない**（§5.3 参照）
2. `tools/scanner/scan_and_select.py` — tkinter チェックリスト GUI
3. `tools/scanner/scan_and_select.py` — plugins.json 出力
4. `scripts/list_plugin_ids.py` — meson 用 id 一覧出力スクリプト

**フェーズ 1: ブリッジ本体**

5. `config/config` — JSON 設定ファイル読み込み・プラグイン EXE パス解決（`plugins.json` 対応含む）
6. `ipc/process` — 子プロセス起動＋パイプ接続
7. `ipc/wire_io` — `PluginSession` + `std::jthread` によるマルチスレッド通信基盤
8. `host/pdb_stubs` — 最小スタブ（上記5種）、`shared_mutex` で保護
9. `host/tile_transfer` — gimp_tile_get / gimp_tile_put（スレッドローカルバッファ）
10. `csp/buffer` — CSP ↔ RGBA 変換
11. `csp/plugin_entry` — CSP プラグインとして統合・複数セッション並列実行

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

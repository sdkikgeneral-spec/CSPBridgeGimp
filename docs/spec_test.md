# CSPBridgeGimp テスト仕様書

## 1. テストフレームワーク

**Catch2**（`subprojects/catch2.wrap`）を使用。header-only で Meson wrap サポートがあり導入が簡単。

```meson
# meson.build に追加
catch2_dep = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])

test_srcs = files(
  'tests/test_config.cpp',
  'tests/test_tile_transfer.cpp',
  'tests/test_buffer.cpp',
  'tests/test_wire_io.cpp',
)

test_exe = executable('cspbridge_tests', test_srcs,
  dependencies: [catch2_dep, nlohmann_json_dep],
  link_with: myGimpHost_lib,
)

test('unit tests', test_exe)
```

`meson test` で全テスト実行。`meson test -v` で詳細出力。単一テストの実行:

```bash
meson test -v --test-args="[config]"   # タグ指定
```

---

## 2. ユニットテスト一覧

| テストファイル | 対象モジュール | テスト内容 |
|---|---|---|
| `tests/test_config.cpp` | `src/config/config` | JSON 読み込み・プレースホルダー展開・ファイル不在時のフォールバック |
| `tests/test_tile_transfer.cpp` | `src/host/tile_transfer` | タイルインデックス計算・タイル分割・端タイルの余白処理・スレッドローカルバッファの独立性 |
| `tests/test_buffer.cpp` | `src/csp/buffer` | CSP バッファ ↔ RGBA 変換の往復一致 |
| `tests/test_wire_io.cpp` | `src/ipc/wire_io` | Wire Protocol メッセージのシリアライズ/デシリアライズ（パイプをモックで代替） |
| `tests/test_concurrency.cpp` | `src/ipc/wire_io` + `src/host/tile_transfer` | 複数 `PluginSession` 並列実行時のデータ競合・書き戻し順序の正当性 |

---

## 3. テストケース例

```cpp
// tests/test_config.cpp
TEST_CASE("ExpandPlaceholders: %APPDATA% on Windows")
{
    std::string path = "%APPDATA%/GIMP/3.0/plug-ins";
    std::string expanded = ExpandPlaceholders(path);
    REQUIRE(expanded.find("%APPDATA%") == std::string::npos);
    REQUIRE(expanded.find("GIMP") != std::string::npos);
}

TEST_CASE("LoadConfig: missing file returns defaults")
{
    auto cfg = LoadConfig("nonexistent.json");
    REQUIRE(!cfg.pluginSearchPaths.empty());
}
```

```cpp
// tests/test_tile_transfer.cpp
TEST_CASE("TileIndex: correct calculation")
{
    // 幅 128px の画像（タイル列数 = 2）
    REQUIRE(TileIndex(0,  0,  128) == 0);
    REQUIRE(TileIndex(64, 0,  128) == 1);
    REQUIRE(TileIndex(0,  64, 128) == 2);
    REQUIRE(TileIndex(64, 64, 128) == 3);
}
```

```cpp
// tests/test_buffer.cpp
TEST_CASE("Buffer roundtrip: RGBA conversion is lossless")
{
    std::vector<uint8_t> original = MakeTestRgba(64, 64);
    auto cspBuf = RgbaToCsp(original);
    auto restored = CspToRgba(cspBuf);
    REQUIRE(original == restored);
}
```

---

### 並行テスト例

```cpp
// tests/test_concurrency.cpp
TEST_CASE("Parallel sessions: no data race on tile writeback")
{
    auto session1 = std::async(std::launch::async, RunFilter, "blur");
    auto session2 = std::async(std::launch::async, RunFilter, "sharpen");
    session1.get();
    session2.get();
    // 書き戻し後のバッファが両セッションで汚染されていないことを確認
    REQUIRE(VerifyBufferIntegrity());
}
```

---

## 4. 統合・E2E 検証

| フェーズ | 内容 |
|---|---|
| IPC 単体 | 軽量な GIMP プラグイン EXE を起動し、Wire Protocol ハンドシェイクが通るか確認 |
| タイル転送 | 単色 RGBA バッファを渡し、フィルター後の出力が期待値と一致するか比較 |
| エンドツーエンド | CSP 上でフィルターメニューを呼び出し、GIMP blur 系フィルターが適用されたレイヤーが得られるか確認 |

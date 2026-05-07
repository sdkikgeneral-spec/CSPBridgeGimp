# GIMP 3 プラグイン EXE 利用可能性調査

調査日: 2026-05-07（最終更新）  
初回調査: 2026-05-06  
調査対象: `C:\Users\sdkik\AppData\Local\Programs\GIMP 3\lib\gimp\3.0\plug-ins\`

---

## 背景

CSPBridgeGimp は GIMP プラグインを EXE として spawn し Wire Protocol で通信する構造のため、
EXE として存在するプラグインのみが使用可能。

GIMP 3 では GIMP 2 の多くのフィルタープラグインが **GEGL 統合**によりコア組み込みとなり、
スタンドアロン EXE として存在しない。次期プラグイン選定前に必ず EXE の存在を確認すること。

---

## GEGL 統合により EXE が消えた主要プラグイン（使用不可）

| カテゴリ | 消えたプラグイン |
|---------|----------------|
| ブラー系 | gauss, blur, sel-gauss, mblur, motion-blur, unsharp-mask |
| シャープ系 | sharpen, edge, laplace, sobel, dog |
| ノイズ系 | noise-hurl, noise-slur, noise-spread, erode, dilate |
| 色調補正 | hue-saturation, curves, levels, posterize, threshold, brightness-contrast, color-balance, color-enhance |
| 歪み系 | spread, ripple, shift, whirl, polar, iwarp, displace, lens-distortion, whirl-pinch, polar-coordinates |
| テクスチャ | cubism, plasma, newsprint, oilify, mosaic, bump-map |
| ピクセル化 | pixelize |

---

## GIMP 3 に残っている filter 系 EXE（使用可能）

```
align-layers       blinds             border-average     checkerboard
cml-explorer       colormap-remap     compose            contrast-retinex
crop-zealous       curve-bend         decompose          depth-merge
despeckle          destripe           flame              fractal-explorer
gfig               gimpressionist     gradient-flare     gradient-map
grid               guillotine         hot                ifs-compose
jigsaw             lighting           map-object         nl-filter
pagecurl           qbist              sample-colorize    selection-to-path
smooth-palette     sparkle            sphere-designer    tile
tile-small         van-gogh-lic       warp               wavelet-decompose
```

合計: 40 本（2026-05-06 時点）

---

## 実装済みプラグイン

| プラグイン | プロシージャ名 | 実装日 | 公開パラメーター |
|-----------|-------------|--------|----------------|
| checkerboard | `plug-in-checkerboard` | フェーズ1 | `check-size`（スライダー）, `check-size-2`（スライダー） |
| despeckle | `plug-in-despeckle` | フェーズ2 | `radius`（スライダー 1–30）|
| blinds | `plug-in-blinds` | フェーズ2 | `angle-displacement`（スライダー 0–90）, `num-segments`（スライダー 1–64）|

### 実装済み引数詳細

#### despeckle

| GIMP 引数 | 型 | CSP UI | 値 |
|---------|----|----|---|
| `radius` | gint (1–30) | スライダー公開 | UI 値 |
| `type` | GimpChoice | 固定 | `"adaptive"` |
| `black` | gint | 固定 | `7` |
| `white` | gint | 固定 | `248` |

#### blinds

| GIMP 引数 | 型 | CSP UI | 値 |
|---------|----|----|---|
| `angle-displacement` | gint (0–90) | スライダー公開 | UI 値 |
| `num-segments` | gint (1–1024) | スライダー公開（max 64 に制限） | UI 値 |
| `orientation` | GimpChoice | 固定 | `"horizontal"` |
| `bg-transparent` | gboolean | 固定 | `0`（FALSE） |

---

## 次期実装候補プラグイン

### destripe（最優先候補）

| 項目 | 値 |
|-----|---|
| EXE | `destripe/destripe.exe` ✓ |
| プロシージャ名 | `plug-in-destripe` |

| GIMP 引数 | 型 | CSP UI | 値 |
|---------|----|----|---|
| `avg-width` | gint (2–100) | スライダー公開 | UI 値 |
| `create-histogram` | gboolean | 固定 | `0`（FALSE） |

> destripe は縦縞ノイズ除去フィルター。引数が少なくシンプルで次の実装に適している。

### その他の候補（将来フェーズ）

| プラグイン | プロシージャ名 | 備考 |
|-----------|-------------|------|
| nl-filter | `plug-in-nl-filter` | 非線形フィルター |
| grid | `plug-in-grid` | グリッドオーバーレイ |
| border-average | `plug-in-border-average` | 境界色平均化 |
| contrast-retinex | `plug-in-retinex` | コントラスト強調（Retinex） |
| sparkle | `plug-in-sparkle` | 光のきらめき効果 |
| tile | `plug-in-tile` | タイル繰り返し |

---

## 実装上の注意

- GimpChoice 型は `GpParam{GpParamType::String, "GimpChoice", "adaptive", 0, 0.0}` 形式で送信
- `stringValue` は `wire_io.h` の `GpParam` 構造体に実装済み
- 新プラグイン追加時: `plugins.json` に追加後 `meson setup --reconfigure build` が必須（`feedback_msvc_build.md` 参照）

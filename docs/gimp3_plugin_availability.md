# GIMP 3 プラグイン EXE 利用可能性調査

調査日: 2026-05-07（最終更新）  
初回調査: 2026-05-06  
調査対象: `C:\Users\sdkik\AppData\Local\Programs\GIMP 3\lib\gimp\3.0\plug-ins\`  
引数調査ソース: `D:\Develop\Projects\gimp\plug-ins\`

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

| プラグイン | プロシージャ名 | 実装フェーズ | 公開パラメーター |
|-----------|-------------|------------|----------------|
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

## 実装可能性分類

| 区分 | 条件 |
|-----|------|
| **A: 容易** | 引数が scalar（gint/gdouble/gboolean/GimpChoice）のみ。他 drawable 参照なし |
| **B: 中程度** | 引数が多いが scalar のみ、または色（GimpRGB）を含む |
| **C: 困難** | 他 drawable/image の入力参照が必要 |
| **D: 不可** | UI 必須 / シリアライズデータ依存 / **複数レイヤー・image 出力**（CSP SDK は 1 offscreen out 固定のため書き戻し先なし） |

| プラグイン | 区分 | 理由 |
|-----------|-----|------|
| crop-zealous | A | 引数なし |
| gradient-map | A | 引数なし（前景色グラデーション使用） |
| tile-small | A | 1 引数 |
| qbist | A | 1 引数（ランダム生成、毎回異なる） |
| destripe | A | 2 引数、全 scalar |
| border-average | A | 2 引数（gint + choice） |
| nl-filter | A | 3 引数、全 scalar |
| hot | A | 3 引数（choice×2 + bool） |
| tile | A | 3 引数（gint×2 + bool） |
| contrast-retinex | A | 4 引数（gint×2 + choice + gdouble） |
| jigsaw | A | 5 引数、全 scalar |
| pagecurl | A | 5 引数（choice×3 + bool + gdouble） |
| sparkle | A | 13 引数、全 scalar |
| grid | B | 色（GimpRGB）引数を含む |
| lighting | B | 多引数だが bump/env drawable は NULL 可 |
| fractal-explorer | B | 19 引数、全 scalar だが多い |
| colormap-remap | C | バイト配列（カラーマップデータ） |
| compose | C | 他 image/drawable 入力が必要 |
| depth-merge | C | 他 drawable×4 が必要 |
| map-object | C | 他 drawable 入力多数 |
| sample-colorize | C | サンプル drawable 入力が必要 |
| van-gogh-lic | C | effect-image drawable 必要 |
| warp | C | warp-map drawable 必要 |
| align-layers | D | 複数レイヤー操作（CSP SDK 不可） |
| cml-explorer | D | パラメーターファイル依存 |
| curve-bend | D | 配列引数（256点フリーハンドカーブ） |
| decompose | D | 複数新規 image 作成（CSP SDK 不可） |
| flame | D | インタラクティブ設定依存 |
| gfig | D | UI 専用エディター |
| gimpressionist | D | プリセット名依存 |
| gradient-flare | D | フレア名依存 |
| guillotine | D | 複数 image 出力（CSP SDK 不可） |
| ifs-compose | D | aux シリアライズデータ依存 |
| selection-to-path | D | パス変換（フィルター用途外） |
| smooth-palette | D | 新規 image 作成（CSP SDK 不可） |
| sphere-designer | D | aux シリアライズデータ依存 |
| wavelet-decompose | D | 複数レイヤー作成（CSP SDK 不可） |

---

## 全プラグイン引数詳細

### 区分 A — 実装容易

#### plug-in-crop-zealous

引数なし。`GIMP_RUN_NONINTERACTIVE` で即動作。

#### plug-in-gradient-map

引数なし。前景→背景グラデーションをグレースケール輝度にマッピング。`GIMP_RUN_NONINTERACTIVE` で即動作。

#### plug-in-qbist

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `anti-aliasing` | gboolean | TRUE/FALSE | TRUE | 固定 | `1`（TRUE） |

> ランダムテクスチャ生成。毎回異なる結果になるため CSP の「再適用」と組み合わせた活用向き。

#### plug-in-tile-small

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `num-tiles` | gint | 2–MAX_SEGS | 2 | スライダー公開 | UI 値 |

#### plug-in-border-average

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `thickness` | gint | 0–MAXINT | 3 | スライダー公開 | UI 値 |
| `bucket-exponent` | GimpChoice | levels-1/levels-2/…/levels-256 | levels-16 | 固定 | `"levels-16"` |

#### plug-in-destripe

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `avg-width` | gint | 2–MAX_AVG | 36 | スライダー公開 | UI 値 |
| `create-histogram` | gboolean | TRUE/FALSE | FALSE | 固定 | `0`（FALSE） |

#### plug-in-hot

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `mode` | GimpChoice | ntsc/pal | ntsc | 固定 | `"ntsc"` |
| `action` | GimpChoice | reduce-luminance/reduce-saturation/blacken | reduce-luminance | 固定 | `"reduce-luminance"` |
| `new-layer` | gboolean | TRUE/FALSE | TRUE | 固定 | `0`（FALSE：現レイヤーに適用） |

#### plug-in-nl-filter

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `alpha` | gdouble | 0–1 | 0.3 | スライダー公開 | UI 値 |
| `radius` | gdouble | 1/3–1 | 1/3 | スライダー公開 | UI 値 |
| `filter` | GimpChoice | alpha-trim/optimal-estimation/edge-enhancement | alpha-trim | 固定 | `"alpha-trim"` |

#### plug-in-tile

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `new-width` | gint | 1–MAXSIZE | 1 | スライダー公開 | UI 値（元画像幅のN倍推奨） |
| `new-height` | gint | 1–MAXSIZE | 1 | スライダー公開 | UI 値（元画像高のN倍推奨） |
| `new-image` | gboolean | TRUE/FALSE | TRUE | 固定 | `0`（FALSE：現画像に適用） |

#### plug-in-contrast-retinex

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `scale` | gint | 5–300 | 240 | スライダー公開 | UI 値 |
| `nscales` | gint | 0–8 | 3 | スライダー公開 | UI 値 |
| `scales-mode` | GimpChoice | uniform/low/high | uniform | 固定 | `"uniform"` |
| `cvar` | gdouble | 0–4 | 1.2 | スライダー公開 | UI 値 |

#### plug-in-jigsaw

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `x` | gint | 1–MAX | 5 | スライダー公開 | UI 値 |
| `y` | gint | 1–MAX | 5 | スライダー公開 | UI 値 |
| `style` | GimpChoice | square/curved | square | 固定 | `"square"` |
| `blend-lines` | gint | 1–MAX | 3 | スライダー公開 | UI 値 |
| `blend-amount` | gdouble | 0–MAX | 0.5 | スライダー公開 | UI 値 |

#### plug-in-pagecurl

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `colors` | GimpChoice | fg-bg/current-gradient/current-gradient-reversed | fg-bg | 固定 | `"fg-bg"` |
| `edge` | GimpChoice | upper-left/upper-right/lower-left/lower-right | lower-right | 固定 | `"lower-right"` |
| `orientation` | GimpChoice | vertical/horizontal | vertical | 固定 | `"vertical"` |
| `shade` | gboolean | TRUE/FALSE | TRUE | 固定 | `1`（TRUE） |
| `opacity` | gdouble | 0–1 | 0 | スライダー公開 | UI 値 |

#### plug-in-sparkle

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `lum-threshold` | gdouble | 0–0.1 | 0.01 | スライダー公開 | UI 値 |
| `flare-inten` | gdouble | 0–1 | 0.5 | スライダー公開 | UI 値 |
| `spike-len` | gint | 1–100 | 20 | スライダー公開 | UI 値 |
| `spike-points` | gint | 1–16 | 4 | スライダー公開 | UI 値 |
| `spike-angle` | gint | -1–360 | 15 | スライダー公開 | UI 値 |
| `density` | gdouble | 0–1 | 1 | スライダー公開 | UI 値 |
| `transparency` | gdouble | 0–1 | 0 | スライダー公開 | UI 値 |
| `random-hue` | gdouble | 0–1 | 0 | 固定 | `0.0` |
| `random-saturation` | gdouble | 0–1 | 0 | 固定 | `0.0` |
| `preserve-luminosity` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `inverse` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `border` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `color-type` | GimpChoice | natural-color/foreground-color/background-color | natural-color | 固定 | `"natural-color"` |

---

### 区分 B — 実装中程度

#### plug-in-fractal-explorer

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `fractal-type` | GimpChoice | mandelbrot/julia/barnsley-1/2/3/spider/man-o-war/lambda/sierpinski | mandelbrot | 固定 | `"mandelbrot"` |
| `xmin` | gdouble | -3–3 | -2 | スライダー公開 | UI 値 |
| `xmax` | gdouble | -3–3 | 2 | スライダー公開 | UI 値 |
| `ymin` | gdouble | -3–3 | -1.5 | スライダー公開 | UI 値 |
| `ymax` | gdouble | -3–3 | 1.5 | スライダー公開 | UI 値 |
| `iter` | gdouble | 1–1000 | 50 | スライダー公開 | UI 値 |
| `cx` | gdouble | -2.5–2.5 | -0.75 | 固定 | デフォルト値 |
| `cy` | gdouble | -2.5–2.5 | -0.2 | 固定 | デフォルト値 |
| `color-mode` | GimpChoice | colormap/gradient | colormap | 固定 | `"colormap"` |
| `red-stretch` | gdouble | 0–1 | 1 | 固定 | デフォルト値 |
| `green-stretch` | gdouble | 0–1 | 1 | 固定 | デフォルト値 |
| `blue-stretch` | gdouble | 0–1 | 1 | 固定 | デフォルト値 |
| `red-mode` | GimpChoice | red-sin/red-cos/red-none | red-cos | 固定 | `"red-cos"` |
| `green-mode` | GimpChoice | green-sin/green-cos/green-none | green-cos | 固定 | `"green-cos"` |
| `blue-mode` | GimpChoice | blue-sin/blue-cos/blue-none | blue-sin | 固定 | `"blue-sin"` |
| `red-invert` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `green-invert` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `blue-invert` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `n-colors` | gint | 2–8192 | 512 | 固定 | デフォルト値 |
| `use-loglog-smoothing` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |

#### plug-in-grid

GimpRGB 型の色引数を含む。CSP から色を渡す仕組みが必要。

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `hwidth` | gint | 0–MAXSIZE | 1 | スライダー公開 | UI 値 |
| `hspace` | gint | 1–MAXSIZE | 16 | スライダー公開 | UI 値 |
| `hoffset` | gint | 0–MAXSIZE | 8 | 固定 | デフォルト値 |
| `hcolor` | GimpRGB | — | (デフォルト) | 固定 | デフォルト（黒） |
| `vwidth` | gint | 0–MAXSIZE | 1 | スライダー公開 | UI 値 |
| `vspace` | gint | 1–MAXSIZE | 16 | スライダー公開 | UI 値 |
| `voffset` | gint | 0–MAXSIZE | 8 | 固定 | デフォルト値 |
| `vcolor` | GimpRGB | — | (デフォルト) | 固定 | デフォルト（黒） |
| `iwidth` | gint | 0–MAXSIZE | 0 | 固定 | `0` |
| `ispace` | gint | 1–MAXSIZE | 2 | 固定 | デフォルト値 |
| `ioffset` | gint | 0–MAXSIZE | 6 | 固定 | デフォルト値 |
| `icolor` | GimpRGB | — | (デフォルト) | 固定 | デフォルト（黒） |

#### plug-in-lighting

bump-drawable・env-drawable は NULL 可（デフォルト動作）。スカラー引数のみで動作可能。

| GIMP 引数 | 型 | 範囲 | デフォルト | CSP UI | 値 |
|---------|----|----|---------|------|----|
| `bump-drawable` | GimpDrawable | — | NULL | 固定 | NULL（バンプマップなし） |
| `env-drawable` | GimpDrawable | — | NULL | 固定 | NULL（環境マップなし） |
| `do-bumpmap` | gboolean | TRUE/FALSE | TRUE | 固定 | `0`（FALSE） |
| `do-envmap` | gboolean | TRUE/FALSE | TRUE | 固定 | `0`（FALSE） |
| `bumpmap-type` | GimpChoice | bumpmap-linear/log/sinusoidal/spherical | bumpmap-linear | 固定 | `"bumpmap-linear"` |
| `bumpmap-max-height` | gdouble | 0–INF | 0.1 | 固定 | デフォルト値 |
| `light-type-1` | GimpChoice | light-none/directional/point/spot | light-point | 固定 | `"light-point"` |
| `light-color-1` | GimpRGB | — | white | 固定 | デフォルト（白） |
| `light-intensity-1` | gdouble | 0–100 | 1 | スライダー公開 | UI 値 |
| `light-position-x-1` | gdouble | -INF–INF | -1 | 固定 | デフォルト値 |
| `light-position-y-1` | gdouble | -INF–INF | -1 | 固定 | デフォルト値 |
| `light-position-z-1` | gdouble | -INF–INF | 1 | 固定 | デフォルト値 |
| `light-direction-x-1` | gdouble | -INF–INF | -1 | 固定 | デフォルト値 |
| `light-direction-y-1` | gdouble | -INF–INF | -1 | 固定 | デフォルト値 |
| `light-direction-z-1` | gdouble | -INF–INF | 1 | 固定 | デフォルト値 |
| `ambient-intensity` | gdouble | 0–1 | 0.2 | スライダー公開 | UI 値 |
| `diffuse-intensity` | gdouble | 0–1 | 0.5 | 固定 | デフォルト値 |
| `diffuse-reflectivity` | gdouble | 0–1 | 0.4 | 固定 | デフォルト値 |
| `specular-reflectivity` | gdouble | 0–1 | 0.5 | スライダー公開 | UI 値 |
| `highlight` | gdouble | 0–INF | 27 | スライダー公開 | UI 値 |
| `metallic` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `antialiasing` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `new-image` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `transparent-background` | gboolean | TRUE/FALSE | FALSE | 固定 | `0` |
| `distance` | gdouble | 0–2 | 0.25 | 固定 | デフォルト値 |

---

### 区分 C — 他 drawable 入力が必要

#### plug-in-colormap-remap

| GIMP 引数 | 型 | 範囲 | デフォルト | 備考 |
|---------|----|----|---------|------|
| `map` | bytes | — | — | カラーマップ差し替えデータ（バイト列）|

#### plug-in-compose

他 image 入力が最大 3 枚必要。単体 drawable フィルターとして利用困難。

#### plug-in-depth-merge

他 drawable を 4 枚入力する必要があり、単体フィルターとして実装困難。

#### plug-in-map-object

box/cylinder の face drawable を NULL にすればスカラーのみで動作する可能性あるが、主用途が 3D マッピングのため実装優先度低。

#### plug-in-sample-colorize

`sample-drawable` として別 drawable 入力が必要。

#### plug-in-van-gogh-lic

`effect-image` drawable 入力が必須のため、単体フィルターとして実装困難。

#### plug-in-warp

`warp-map` drawable 入力が必須のため、単体フィルターとして実装困難。

---

### 区分 D — 実装不可

#### plug-in-align-layers

レイヤー操作のため CSP SDK（1 offscreen out 固定）と非互換。実装不可。

#### plug-in-cml-explorer

パラメーターファイル依存のため NONINTERACTIVE での安定動作は困難。

| GIMP 引数 | 型 | 備考 |
|---------|----|----|
| `parameter-file` | GimpFile | CML_explorer 設定ファイルパス |

#### plug-in-curve-bend

256 点フリーハンドカーブのバイト列引数を含むため NONINTERACTIVE での制御は困難。

#### plug-in-decompose

カラーチャンネルごとに新規 image を 3〜4 枚作成する。CSP SDK は 1 offscreen out 固定のため書き戻し先がなく実装不可。

#### plug-in-flame

インタラクティブな炎パラメーター設定に依存。NONINTERACTIVE での実用は困難。

#### plug-in-gfig

UI 専用の幾何図形エディター。引数なし・NONINTERACTIVE 不可。

#### plug-in-gimpressionist

プリセット名依存。あらかじめ保存されたプリセットが必要。

#### plug-in-gradient-flare

フレア名（`gflare-name`）依存。カスタムグラデーションフレア定義が前提。

#### plug-in-guillotine

ガイドに沿って画像を複数 image に分割する。CSP SDK は 1 offscreen out 固定のため実装不可。

#### plug-in-ifs-compose

aux シリアライズデータ依存。NONINTERACTIVE での利用は実質不可。

#### plug-in-selection-to-path

選択範囲をパスに変換する操作。フィルター用途外。

#### plug-in-smooth-palette

新規 image（パレット画像）を作成する。CSP SDK は 1 offscreen out 固定のため実装不可。

#### plug-in-sphere-designer

aux シリアライズデータ依存。NONINTERACTIVE での利用は実質不可。

#### plug-in-wavelet-decompose

周波数帯ごとに複数のレイヤーを作成する。CSP SDK は 1 offscreen out 固定のため実装不可。

---

## 推奨実装順（区分 A、用途のあるもの）

| 優先度 | プラグイン | プロシージャ名 | 特徴 |
|-------|-----------|-------------|------|
| 1 | destripe | `plug-in-destripe` | 縦縞除去、引数 2 個 |
| 2 | nl-filter | `plug-in-nl-filter` | 非線形ノイズ除去、引数 3 個 |
| 3 | contrast-retinex | `plug-in-retinex` | コントラスト強調、引数 4 個 |
| 4 | jigsaw | `plug-in-jigsaw` | ジグソーテクスチャ、引数 5 個 |
| 5 | hot | `plug-in-hot` | NTSC/PAL セーフカラー補正、引数 3 個 |
| 6 | sparkle | `plug-in-sparkle` | きらめきエフェクト、引数 13 個 |
| 7 | border-average | `plug-in-border-average` | 境界色平均化、引数 2 個 |
| 8 | tile | `plug-in-tile` | タイル繰り返し、引数 3 個 |
| 9 | tile-small | `plug-in-tile-small` | 小タイル、引数 1 個 |
| 10 | pagecurl | `plug-in-pagecurl` | ページめくり効果、引数 5 個 |
| 11 | fractal-explorer | `plug-in-fractal-explorer` | フラクタル生成、引数多（区分 B） |

---

## 実装上の注意

- GimpChoice 型は `GpParam{GpParamType::String, "GimpChoice", "value", 0, 0.0}` 形式で送信
- `stringValue` は `wire_io.h` の `GpParam` 構造体に実装済み
- GimpRGB 型は未実装。`plug-in-grid` 等を実装する際は `wire_io.h` への追加が必要
- 新プラグイン追加時: `plugins.json` に追加後 `meson setup --reconfigure build` が必須（`feedback_msvc_build.md` 参照）

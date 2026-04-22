GIMP wrap overlay placeholder
==============================

この階層は `subprojects/gimp.wrap` をフル有効化する際の overlay meson.build
などを置く場所。

現 PoC フェーズでは `subprojects/gimp.wrap` は作成していない。
libgimpwire / libgimpbase はシステムインストール済み GIMP 3 を pkg-config
経由で参照する方針（meson.build の dependency() を required:false で呼ぶ）。

将来 wrap 化する場合の TODO:
  1. `subprojects/gimp.wrap` を新規作成し、以下のように設定:

        [wrap-git]
        url      = https://gitlab.gnome.org/GNOME/gimp.git
        revision = GIMP_3_0_0
        depth    = 1
        patch_directory = gimp

        [provide]
        libgimpwire  = libgimpwire_dep
        libgimpbase  = libgimpbase_dep

  2. ここ (`subprojects/packagefiles/gimp/`) に overlay `meson.build` を置き、
     `libgimpbase/` と `libgimpwire/` 配下の .c のみを static_library() として
     ビルドする（GIMP 本体 meson のサブツリーは使わない）。
  3. glib-2.0 / gobject-2.0 / gio-2.0 はシステムから引く。
  4. app/, gimp/ 以下 (GPL v3) はビルドしない。libgimp{base,wire} (LGPL v3)
     のみを対象にする。
  5. meson.build 側の `libgimpwire_dep` / `libgimpbase_dep` 定義に
     `fallback: ['gimp', 'libgimpwire_dep']` を追加する。

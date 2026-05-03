"""
@file   scan_and_select.py
@brief  src/plugins/*.cpp をスキャンして plugins.json を生成する選択ツール
@author CSPBridgeGimp
@date   2026-05-03

このスクリプトは `meson setup` の前に **手動で 1 回** 実行する。
`src/plugins/` に実装済みのプラグイン cpp ファイルを走査し、
tkinter チェックリスト GUI でユーザーが選択したプラグインを
`plugins.json` に書き出す。

処理フロー:
    1. src/plugins/ を走査し、*.cpp ファイルを列挙（plugin_iface.h を除く）
    2. 各ファイルから id を推定:
       ファイル名ステム → '_' で分割 → 各パーツを capitalize → join
       → 'PlugIn' を先頭に付加
       例: checkerboard.cpp → PlugInCheckerboard
       例: gauss_blur.cpp   → PlugInGaussBlur
    3. tkinter チェックリスト GUI で一覧表示
    4. 選択したものを plugins.json（id + src パス）に書き出す

Wire Protocol スキャンロジック（旧機能）は
tools/dev/discover_gimp_plugins.py に移管済み。

CLI オプション:
    --src-dir  スキャン対象の src/plugins/ パス
               （デフォルト: スクリプトから 2 階層上の src/plugins/）
    --output   plugins.json の出力先
               （デフォルト: プロジェクトルート）
    --no-gui   GUI なしで全プラグインを選択
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Optional


# ---------------------------------------------------------------------------
# プラグイン情報
# ---------------------------------------------------------------------------


class PluginCandidate:
    """src/plugins/ 内の 1 つの .cpp ファイルに対応するプラグイン候補。"""

    def __init__(self, cpp_path: pathlib.Path, project_root: pathlib.Path) -> None:
        self.cpp_path = cpp_path
        self.project_root = project_root

    @property
    def plugin_id(self) -> str:
        """
        @brief  ファイル名ステムから plugins.json 用 id を生成する。

        例: checkerboard → PlugInCheckerboard
        例: gauss_blur   → PlugInGaussBlur
        """
        stem = self.cpp_path.stem  # 例: "checkerboard", "gauss_blur"
        parts = stem.split('_')
        capitalized = ''.join(p.capitalize() for p in parts if p)
        return 'PlugIn' + capitalized

    @property
    def src_rel(self) -> str:
        """
        @brief  プロジェクトルートからの相対パス（スラッシュ区切り）。

        meson.build の files() に渡す文字列として使用する。
        """
        try:
            rel = self.cpp_path.relative_to(self.project_root)
            return rel.as_posix()
        except ValueError:
            return self.cpp_path.as_posix()

    @property
    def filename(self) -> str:
        """@brief ファイル名（表示用）。"""
        return self.cpp_path.name

    def to_json_entry(self) -> dict:
        """@brief plugins.json エントリ dict を返す。"""
        return {
            'id':  self.plugin_id,
            'src': self.src_rel,
        }


# ---------------------------------------------------------------------------
# スキャン
# ---------------------------------------------------------------------------

# plugin_iface.h と同名のステムは id 推定の対象外
_EXCLUDE_STEMS = frozenset({'plugin_iface'})


def scan_plugins(src_dir: pathlib.Path, project_root: pathlib.Path) -> list[PluginCandidate]:
    """
    @brief  src/plugins/ の *.cpp を列挙して PluginCandidate リストを返す。

    @param  src_dir       スキャン対象ディレクトリ
    @param  project_root  プロジェクトルート（相対パス計算に使用）
    @return PluginCandidate のリスト（ファイル名昇順）
    """
    if not src_dir.is_dir():
        return []

    candidates: list[PluginCandidate] = []
    for cpp in sorted(src_dir.glob('*.cpp')):
        if cpp.stem in _EXCLUDE_STEMS:
            continue
        candidates.append(PluginCandidate(cpp, project_root))
    return candidates


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------


def select_plugins_gui(
    candidates: list[PluginCandidate],
) -> Optional[list[PluginCandidate]]:
    """
    @brief  tkinter チェックリストで選択 UI を出す。

    @param  candidates  スキャンで見つかったプラグイン候補リスト
    @return 選択された PluginCandidate のリスト。キャンセル時は None
    """
    if not candidates:
        root = tk.Tk()
        root.withdraw()
        messagebox.showinfo(
            'CSPBridgeGimp Scanner',
            'No plugin .cpp files found in src/plugins/.\n'
            'Implement a plugin and re-run this script.',
        )
        root.destroy()
        return None

    state: dict[str, Optional[list[PluginCandidate]]] = {'result': None}

    root = tk.Tk()
    root.title('CSPBridgeGimp Plugin Scanner')
    root.geometry('720x400')

    header = ttk.Label(
        root,
        text=(
            f'Found {len(candidates)} plugin(s) in src/plugins/. '
            'Check the ones to include in plugins.json.'
        ),
    )
    header.pack(anchor='w', padx=8, pady=(8, 4))

    container = ttk.Frame(root)
    container.pack(fill='both', expand=True, padx=8, pady=4)

    canvas = tk.Canvas(container, highlightthickness=0)
    scrollbar = ttk.Scrollbar(container, orient='vertical', command=canvas.yview)
    inner = ttk.Frame(canvas)

    inner.bind(
        '<Configure>',
        lambda e: canvas.configure(scrollregion=canvas.bbox('all')),
    )
    canvas.create_window((0, 0), window=inner, anchor='nw')
    canvas.configure(yscrollcommand=scrollbar.set)

    canvas.pack(side='left', fill='both', expand=True)
    scrollbar.pack(side='right', fill='y')

    # カラムヘッダー
    header_frame = ttk.Frame(inner)
    header_frame.grid(row=0, column=0, sticky='ew', padx=2, pady=(0, 4))
    ttk.Label(header_frame, text='', width=3).grid(row=0, column=0)
    ttk.Label(header_frame, text='File', width=30, anchor='w').grid(
        row=0, column=1, sticky='w'
    )
    ttk.Label(header_frame, text='Plugin ID', width=36, anchor='w').grid(
        row=0, column=2, sticky='w'
    )

    check_vars: list[tk.BooleanVar] = []
    for i, cand in enumerate(candidates):
        var = tk.BooleanVar(value=False)
        check_vars.append(var)
        row_frame = ttk.Frame(inner)
        row_frame.grid(row=i + 1, column=0, sticky='ew', padx=2, pady=1)

        ttk.Checkbutton(row_frame, variable=var).grid(row=0, column=0, sticky='w')
        ttk.Label(row_frame, text=cand.filename, width=30, anchor='w').grid(
            row=0, column=1, sticky='w'
        )
        ttk.Label(row_frame, text=cand.plugin_id, width=36, anchor='w').grid(
            row=0, column=2, sticky='w'
        )

    btn_frame = ttk.Frame(root)
    btn_frame.pack(fill='x', padx=8, pady=8)

    def on_save() -> None:
        selected = [c for c, v in zip(candidates, check_vars) if v.get()]
        if not selected:
            if not messagebox.askyesno(
                'Confirm',
                'No plugins selected. Save an empty plugins.json?',
            ):
                return
        state['result'] = selected
        root.destroy()

    def on_cancel() -> None:
        state['result'] = None
        root.destroy()

    def on_select_all() -> None:
        for v in check_vars:
            v.set(True)

    def on_select_none() -> None:
        for v in check_vars:
            v.set(False)

    ttk.Button(btn_frame, text='Select All', command=on_select_all).pack(side='left')
    ttk.Button(btn_frame, text='Select None', command=on_select_none).pack(
        side='left', padx=(4, 0)
    )
    ttk.Button(btn_frame, text='Cancel', command=on_cancel).pack(side='right')
    ttk.Button(btn_frame, text='Save plugins.json', command=on_save).pack(
        side='right', padx=(0, 4)
    )

    root.protocol('WM_DELETE_WINDOW', on_cancel)
    root.mainloop()

    return state['result']


# ---------------------------------------------------------------------------
# plugins.json 書き出し
# ---------------------------------------------------------------------------


def write_plugins_json(output_path: pathlib.Path, selected: list[PluginCandidate]) -> None:
    """@brief  plugins.json を書き出す（UTF-8、2 スペースインデント）。"""
    entries = [c.to_json_entry() for c in selected]
    with open(output_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(entries, f, ensure_ascii=False, indent=2)
        f.write('\n')


# ---------------------------------------------------------------------------
# エントリポイント
# ---------------------------------------------------------------------------


def main() -> int:
    """@brief  CLI エントリ。成功時 0、キャンセル時 1。"""
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    default_src_dir = repo_root / 'src' / 'plugins'
    default_output = repo_root / 'plugins.json'

    parser = argparse.ArgumentParser(
        description=(
            'Scan src/plugins/*.cpp and write plugins.json. '
            'Run this before `meson setup`.'
        )
    )
    parser.add_argument(
        '--src-dir',
        default=str(default_src_dir),
        help='Path to src/plugins/ directory (default: %(default)s)',
    )
    parser.add_argument(
        '--output',
        default=str(default_output),
        help='Path to write plugins.json (default: %(default)s)',
    )
    parser.add_argument(
        '--no-gui',
        action='store_true',
        help='Skip the selection GUI and auto-select all plugins.',
    )

    args = parser.parse_args()

    src_dir = pathlib.Path(args.src_dir).resolve()
    output_path = pathlib.Path(args.output)

    print(f'[scan] src_dir={src_dir}', file=sys.stderr)
    print(f'[scan] output={output_path}', file=sys.stderr)

    candidates = scan_plugins(src_dir, repo_root)
    print(f'[scan] found {len(candidates)} plugin(s)', file=sys.stderr)
    for cand in candidates:
        print(f'[scan]   {cand.filename} -> {cand.plugin_id}', file=sys.stderr)

    if args.no_gui:
        selected: Optional[list[PluginCandidate]] = candidates
    else:
        selected = select_plugins_gui(candidates)
        if selected is None:
            print('[scan] cancelled by user', file=sys.stderr)
            return 1

    write_plugins_json(output_path, selected)
    print(
        f'[scan] wrote {len(selected)} plugin(s) to {output_path}',
        file=sys.stderr,
    )
    return 0


if __name__ == '__main__':
    sys.exit(main())

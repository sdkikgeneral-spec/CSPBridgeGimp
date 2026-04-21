"""
meson setup 時に bridge_config.json を読み込み、
プレースホルダーを展開して指定キーの値を stdout に出力する。
"""

import argparse
import json
import os
import sys


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--config',   required=True, help='bridge_config.json のパス')
    parser.add_argument('--platform', required=True, help="'windows' or 'darwin'")
    parser.add_argument('--key',      required=True, help='取得するキー名')
    args = parser.parse_args()

    try:
        with open(args.config, encoding='utf-8') as f:
            cfg = json.load(f)
    except FileNotFoundError:
        print(f'[read_config] 設定ファイルが見つかりません: {args.config}', file=sys.stderr)
        sys.exit(1)

    platform_key = 'mac' if args.platform == 'darwin' else 'windows'

    try:
        value = cfg[platform_key][args.key]
    except KeyError as e:
        print(f'[read_config] キーが見つかりません: {e}', file=sys.stderr)
        sys.exit(1)

    # Windows: %FOO% 形式を環境変数展開
    # Mac: {HOME} を展開
    value = os.path.expandvars(value)
    value = value.replace('{HOME}', os.path.expanduser('~'))

    print(value)


if __name__ == '__main__':
    main()

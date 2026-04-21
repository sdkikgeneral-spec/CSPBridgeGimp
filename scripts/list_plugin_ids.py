"""
meson setup 時に plugins.json から id 一覧を stdout に出力するスクリプト。
meson.build の run_command() から呼び出される。
"""
import json
import sys

plugins_json = sys.argv[1]

with open(plugins_json, encoding='utf-8') as f:
    data = json.load(f)

print('\n'.join(entry['id'] for entry in data))

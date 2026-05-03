"""
meson setup 時に plugins.json から各プラグインの情報を stdout に出力するスクリプト。
meson.build の run_command() から呼び出される。

出力形式: 1 プラグインにつき 1 行、フィールドを | で区切る
  <id>|<src>
  例: PlugInCheckerboard|src/plugins/checkerboard.cpp
"""
import json
import sys

plugins_json = sys.argv[1]

with open(plugins_json, encoding='utf-8') as f:
    data = json.load(f)

for entry in data:
    plugin_id = entry['id']
    src = entry['src']
    print(f'{plugin_id}|{src}')

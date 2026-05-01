"""
meson setup 時に plugins.json から各プラグインの情報を stdout に出力するスクリプト。
meson.build の run_command() から呼び出される。

出力形式: 1 プラグインにつき 1 行、フィールドを | で区切る
  <id>|<exe_base>|<procedure>
  例: PlugInCheckerboard|checkerboard|plug-in-checkerboard
"""
import json
import sys

plugins_json = sys.argv[1]

with open(plugins_json, encoding='utf-8') as f:
    data = json.load(f)

for entry in data:
    plugin_id = entry['id']
    exe_base = entry['exe'].removesuffix('.exe')
    procedure = entry['procedure']
    print(f'{plugin_id}|{exe_base}|{procedure}')

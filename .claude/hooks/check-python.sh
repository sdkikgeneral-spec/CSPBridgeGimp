#!/usr/bin/env bash
#
# PostToolUse hook: .py ファイル編集後に python -m py_compile で構文チェック。
# 失敗時は exit 2 で Claude に通知。
#
# stdin: { "tool_input": { "file_path": "..." }, ... }

FILE=$(python -c "import sys, json; d = json.load(sys.stdin); print(d.get('tool_input', {}).get('file_path', ''))" 2>/dev/null)

if [ -z "$FILE" ]; then
    exit 0
fi

case "$FILE" in
    *.py) ;;
    *) exit 0 ;;
esac

# ファイルが存在しない（削除直後など）はスキップ
if [ ! -f "$FILE" ]; then
    exit 0
fi

OUT=$(python -m py_compile "$FILE" 2>&1)
STATUS=$?

if [ $STATUS -ne 0 ]; then
    echo "[check-python] py_compile failed for $FILE:" >&2
    echo "$OUT" >&2
    exit 2
fi

exit 0

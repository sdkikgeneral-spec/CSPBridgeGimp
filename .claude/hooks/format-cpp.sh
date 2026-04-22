#!/usr/bin/env bash
#
# PostToolUse hook: .cpp / .h / .hpp 等を編集したら clang-format -i を自動実行。
# clang-format が PATH に無い環境では何もせず成功終了（best-effort）。
#
# stdin: { "tool_input": { "file_path": "..." }, ... }

FILE=$(python -c "import sys, json; d = json.load(sys.stdin); print(d.get('tool_input', {}).get('file_path', ''))" 2>/dev/null)

if [ -z "$FILE" ]; then
    exit 0
fi

case "$FILE" in
    *.cpp|*.cc|*.cxx|*.h|*.hpp|*.hxx) ;;
    *) exit 0 ;;
esac

if ! command -v clang-format >/dev/null 2>&1; then
    # 未インストール時は警告だけ出して成功扱い（CI/別環境対応）
    echo "[format-cpp] clang-format not in PATH; skipped formatting $FILE" >&2
    exit 0
fi

clang-format -i "$FILE" 2>&1 || {
    echo "[format-cpp] clang-format failed for $FILE (non-fatal)" >&2
    exit 0
}

exit 0

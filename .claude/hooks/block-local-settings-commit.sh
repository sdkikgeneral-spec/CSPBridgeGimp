#!/usr/bin/env bash
#
# PreToolUse hook: Bash で `.claude/settings.local.json` を git add / commit に
# 巻き込もうとしたらブロックする。このファイルはセッション中に動的に追加される
# 権限キャッシュであり、コミット履歴に混ざるとノイズになる。
#
# 明示的なパス指定のみブロックする（git add -A / git add . は警告のみ）。
#
# stdin: { "tool_input": { "command": "..." }, ... }

CMD=$(python -c "import sys, json; d = json.load(sys.stdin); print(d.get('tool_input', {}).get('command', ''))" 2>/dev/null)

if [ -z "$CMD" ]; then
    exit 0
fi

# パス区切り正規化（Windows バックスラッシュ対策）
NORM=$(echo "$CMD" | tr '\\' '/')

# 明示的に settings.local.json を add / commit しようとしている → ブロック
if echo "$NORM" | grep -qE 'git[[:space:]]+(add|commit)[^|&;]*\.claude/settings\.local\.json'; then
    cat >&2 <<'EOF'
[block-local-settings-commit] .claude/settings.local.json は手元セッションの
権限キャッシュであり、コミットするとチームにノイズが混ざります。

共有したいフック・権限は .claude/settings.json に書いてください。
どうしても settings.local.json をコミットする必要がある場合は、ユーザーに
確認を取ってから一時的にこのフックを無効化してください。
EOF
    exit 2
fi

# 大雑把な一括 add（-A / --all / 無引数 git add）で settings.local.json が
# 変更済みかつ staged 対象に入る場合は警告のみ（止めない）
if echo "$NORM" | grep -qE 'git[[:space:]]+add[[:space:]]+(-A|--all|-\.|[[:space:]]*\.$)'; then
    if git diff --quiet -- .claude/settings.local.json 2>/dev/null; then
        : # 変更なし、静かに通過
    else
        echo "[block-local-settings-commit] warning: .claude/settings.local.json has unstaged changes that may be picked up by '$CMD'." >&2
        echo "[block-local-settings-commit] consider staging specific paths instead of '-A' / '.'." >&2
        # 警告のみで exit 0（ユーザー意図尊重）
    fi
fi

exit 0

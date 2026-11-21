#!/usr/bin/env bash
# Workspace install mode (containers reached through a browser terminal):
# sets Herd up in this workspace. Safe to run again.
set -eu
H="$HOME"; SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$H/.local/bin" "$H/.config/herd" "$H/.config/opencode/plugins" "$H/.claude"

ln -sf "$SRC/../bin/herd" "$H/.local/bin/herd"
[ -x "$H/.local/bin/fzf" ] || command -v fzf >/dev/null || install -m 755 "$SRC/fzf" "$H/.local/bin/fzf"
for f in agent-board agent-roster; do ln -sf "$H/forge/agent-sidebar/bin/$f" "$H/.local/bin/$f"; done
cp "$SRC/tmux.conf" "$H/.config/herd/tmux.conf"
ln -sf "$H/forge/agent-sidebar/producers/opencode/plugin.js" "$H/.config/opencode/plugins/agent-sidebar.js"

line='source-file -q ~/.config/herd/tmux.conf'
[ -L "$H/.tmux.conf" ] && rm "$H/.tmux.conf"
grep -qsF "$line" "$H/.tmux.conf" || printf '%s\n' "$line" >>"$H/.tmux.conf"

keep='mkdir -p /tmp/agent-sidebar-state 2>/dev/null # herd: its state lives on local disk'
for rc in .zshenv .bashrc .profile; do
	grep -qsF '# herd: its state' "$H/$rc" || printf '%s\n' "$keep" >>"$H/$rc"
done
mkdir -p /tmp/agent-sidebar-state "$H/.local/state"
if [ ! -L "$H/.local/state/agent-sidebar" ]; then
	[ -e "$H/.local/state/agent-sidebar" ] && mv "$H/.local/state/agent-sidebar" "$H/.local/state/agent-sidebar.before-herd"
	ln -s /tmp/agent-sidebar-state "$H/.local/state/agent-sidebar"
fi

python3 - "$H" <<'PY'
import json, os, sys
h = sys.argv[1]
f = h + "/.claude/settings.json"
try:
    s = json.load(open(f))
except (OSError, ValueError):
    s = {}
hooks = s.setdefault("hooks", {})
want = {"SessionStart": h + "/forge/agent-sidebar/bin/agent-board-hook", "PostToolUse": h + "/forge/agent-sidebar/bin/agent-board-hook",
        "UserPromptSubmit": h + "/forge/agent-sidebar/bin/agent-board hook"}
for event, cmd in want.items():
    rows = hooks.setdefault(event, [])
    if not any(x.get("command") == cmd for r in rows for x in r.get("hooks", [])):
        rows.append({"hooks": [{"type": "command", "command": cmd, "timeout": 5}]})
tmp = f + ".herd"
json.dump(s, open(tmp, "w"), indent=2)
os.replace(tmp, f)
PY

for doc in "$H/.claude/CLAUDE.md" "$H/.config/opencode/AGENTS.md"; do
	grep -qsF '<!-- herd -->' "$doc" || { [ -s "$doc" ] && printf '\n' >>"$doc"; cat "$SRC/../briefing.md" >>"$doc"; }
done

tmux has-session 2>/dev/null && tmux source-file "$H/.config/herd/tmux.conf" 2>/dev/null || true
if "$H/.local/bin/herd" doctor; then
	printf '\nHerd is set up. Type: herd\n'
else
	printf '\nSomething above needs fixing before Herd will work.\n'; exit 1
fi

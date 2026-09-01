#!/usr/bin/env sh
# Agent picker: a popup listing every agent, with a live preview of its screen.
# Enter jumps to one, ctrl-x kills one.
#
# The interaction design here - a popup fzf picker over the agent list, with a
# capture-pane preview and ctrl-x to kill - follows
# craftzdog/tmux-claude-session-manager by Takuya Matsuyama, MIT licensed.
# The implementation is independent: rows come from agent-sidebar's own loader,
# which reads the per-pid state files directly (about 6ms) rather than shelling
# out to the agent CLI (about 400ms).
set -u

DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/agent-sidebar"

[ -x "$BIN" ] || { tmux display-message "agent-sidebar: not built - run make in $DIR"; exit 0; }
for tool in fzf tmux; do
	command -v "$tool" >/dev/null 2>&1 || {
		tmux display-message "agent-sidebar: $tool is required for the picker"
		exit 0
	}
done

# Columns: 1 pid, 2 pane id, 3 state, 4 age, 5 session, 6 path.
# The first two are hidden; they are what the actions key on.
sel=$("$BIN" --list | fzf --ansi --delimiter='\t' --with-nth=3.. \
	--reverse --cycle --no-sort \
	--header='enter: jump    ctrl-x: kill    ctrl-p: park' \
	--preview='tmux capture-pane -ept {2} 2>/dev/null || echo "(not in a pane)"' \
	--preview-window='up,70%,follow' \
	--bind="ctrl-x:execute-silent($BIN --kill {1})+reload(sleep 0.3; $BIN --list)" \
	--bind="ctrl-p:execute-silent($BIN --park {1})+reload($BIN --list)")

[ -n "$sel" ] || exit 0
pid=$(printf '%s' "$sel" | cut -f1)
"$BIN" --jump "$pid"

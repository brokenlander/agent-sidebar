#!/usr/bin/env sh
# Toggle the sidebar in one window. Idempotent: a second press closes it, so
# a window can never end up with two.
set -u
DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/agent-sidebar"
win="${1:-}"

width="$(tmux show-option -gqv @agent_sidebar_width)"
[ -z "$width" ] && width=28

existing="$(tmux list-panes -t "$win" -F '#{pane_id} #{pane_current_command}' 2>/dev/null |
	awk '$2 == "agent-sidebar" { print $1; exit }')"

if [ -n "$existing" ]; then
	tmux kill-pane -t "$existing"
	exit 0
fi

if [ ! -x "$BIN" ]; then
	tmux display-message "agent-sidebar: not built - run 'make' in $DIR"
	exit 0
fi

# -b puts it on the left, -d keeps focus where it was
tmux split-window -h -b -l "$width" -d -t "$win" "$BIN"

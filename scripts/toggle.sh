#!/usr/bin/env sh
# One control for every session. Pressed in a window without a sidebar, it
# opens one in every window of every session; pressed in a window with one, it
# closes them all. The choice is kept in @agent_sidebar_open so a window or
# session created afterwards starts the same way, through ensure.sh.
set -u
. "$(dirname "$0")/common.sh"
win="${1:-}"

if [ ! -x "$BIN" ]; then
	tmux display-message "agent-sidebar: not built - run 'make' in $DIR"
	exit 0
fi

if [ -n "$(sidebar_in "$win")" ]; then
	tmux set-option -g @agent_sidebar_open 0
	for w in $(tmux list-windows -a -F '#{window_id}'); do
		close_in "$w"
	done
else
	tmux set-option -g @agent_sidebar_open 1
	for w in $(tmux list-windows -a -F '#{window_id}'); do
		open_in "$w"
	done
fi

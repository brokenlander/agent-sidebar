# Shared by toggle.sh and ensure.sh: where the binary is, how wide a sidebar
# is, and how to open or close one in a window. POSIX sh.
DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/agent-sidebar"

width="$(tmux show-option -gqv @agent_sidebar_width)"
[ -z "$width" ] && width=28

# The toggle and the window-linked hook can reach a new window at the same
# moment, and each would find it empty and split it. One lock around both
# keeps a window to one sidebar.
lock="${XDG_RUNTIME_DIR:-/tmp}/agent-sidebar-$(id -u).lock"
if command -v flock >/dev/null 2>&1; then
	exec 9>"$lock"
	flock 9
fi

# The sidebar pane in a window, if it has one.
sidebar_in() {
	tmux list-panes -t "$1" -F '#{pane_id} #{pane_current_command}' 2>/dev/null |
		awk '$2 == "agent-sidebar" { print $1; exit }'
}

open_in() {
	[ -n "$(sidebar_in "$1")" ] && return 0
	# -b puts it on the left, -d keeps focus where it was
	tmux split-window -h -b -l "$width" -d -t "$1" "$BIN"
}

close_in() {
	p="$(sidebar_in "$1")"
	[ -z "$p" ] && return 0
	# Never take a window's last pane: that closes the window, and with it a
	# session whose agent has already gone.
	[ "$(tmux display-message -p -t "$1" '#{window_panes}')" -gt 1 ] || return 0
	tmux kill-pane -t "$p"
}

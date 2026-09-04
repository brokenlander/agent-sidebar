#!/usr/bin/env sh
# Give a new window a sidebar while they are open everywhere. Run from the
# window-linked hook, which fires for the first window of a new session as
# well as for new-window, and is the only creation hook that names the window.
set -u
. "$(dirname "$0")/common.sh"

[ "$(tmux show-option -gqv @agent_sidebar_open)" = 1 ] || exit 0
[ -n "${1:-}" ] || exit 0
[ -x "$BIN" ] || exit 0
open_in "$1"

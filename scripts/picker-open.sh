#!/usr/bin/env sh
# Opens the picker in a popup sized to its content.
#
# A fixed-height popup leaves the lower half empty whenever there are fewer
# agents than rows, because fzf fills from the top under --reverse. The height
# is computed from the agent count instead, and capped so a large fleet still
# fits on screen.
set -u
DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/agent-sidebar"

[ -x "$BIN" ] || { tmux display-message "agent-sidebar: not built - run make in $DIR"; exit 0; }

agents=$("$BIN" --list 2>/dev/null | wc -l)
[ "$agents" -lt 1 ] && agents=1

# list rows + header + fzf's prompt/counter + the popup border
want=$((agents + 4))

client_h=$(tmux display-message -p '#{client_height}' 2>/dev/null || echo 40)
max=$((client_h * 90 / 100))
min=10
[ "$want" -gt "$max" ] && want=$max
[ "$want" -lt "$min" ] && want=$min

width="$(tmux show-option -gqv @agent_sidebar_picker_width)"
[ -z "$width" ] && width='85%'

tmux display-popup -w "$width" -h "$want" -E "$DIR/scripts/picker.sh"

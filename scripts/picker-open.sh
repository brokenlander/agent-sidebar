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

# Proportional to the client, not to the agent count. Content-sizing was a
# workaround for fzf occupying only part of the popup; with FZF_DEFAULT_OPTS
# cleared it fills whatever it is given, so a taller popup simply means a
# taller preview.
width="$(tmux show-option -gqv @agent_sidebar_picker_width)"
[ -z "$width" ] && width='85%'
height="$(tmux show-option -gqv @agent_sidebar_picker_height)"
[ -z "$height" ] && height='90%'

tmux display-popup -w "$width" -h "$height" -E "$DIR/scripts/picker.sh"

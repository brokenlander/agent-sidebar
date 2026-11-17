#!/usr/bin/env sh
# Opens the picker in a popup, 95% of the client each way unless
# @agent_sidebar_picker_width/height say otherwise.
set -u
DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/agent-sidebar"

[ -x "$BIN" ] || { tmux display-message "agent-sidebar: not built - run make in $DIR"; exit 0; }

# Proportional to the client, not to the agent count. Content-sizing was a
# workaround for fzf occupying only part of the popup; with FZF_DEFAULT_OPTS
# cleared it fills whatever it is given, so a taller popup simply means a
# taller preview.
width="$(tmux show-option -gqv @agent_sidebar_picker_width)"
[ -z "$width" ] && width='95%'
height="$(tmux show-option -gqv @agent_sidebar_picker_height)"
[ -z "$height" ] && height='95%'

tmux display-popup -w "$width" -h "$height" -E "$DIR/scripts/picker.sh"

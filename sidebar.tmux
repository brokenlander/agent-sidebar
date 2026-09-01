#!/usr/bin/env bash
# TPM entry point: installs the toggle binding.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

key="$(tmux show-option -gqv @agent_sidebar_key)"
[ -z "$key" ] && key='e'

tmux bind-key "$key" run-shell "$DIR/scripts/toggle.sh '#{q:window_id}'"

# The picker opens in a popup, so it is bound to display-popup directly rather
# than going through run-shell.
picker_key="$(tmux show-option -gqv @agent_sidebar_picker_key)"
[ -z "$picker_key" ] && picker_key='g'
pw="$(tmux show-option -gqv @agent_sidebar_picker_width)";  [ -z "$pw" ] && pw='80%'
ph="$(tmux show-option -gqv @agent_sidebar_picker_height)"; [ -z "$ph" ] && ph='80%'
tmux bind-key "$picker_key" display-popup -w "$pw" -h "$ph" -E "$DIR/scripts/picker.sh"

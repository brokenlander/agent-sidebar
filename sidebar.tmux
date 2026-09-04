#!/usr/bin/env bash
# TPM entry point: installs the toggle binding.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

key="$(tmux show-option -gqv @agent_sidebar_key)"
[ -z "$key" ] && key='e'

tmux bind-key "$key" run-shell "$DIR/scripts/toggle.sh '#{q:window_id}'"

# A window or session created later comes up the way the control is set.
# window-linked fires for the first window of a new session as well as for
# new-window, and is the only creation hook that names the window. Index 1
# leaves a hook the user set at index 0 alone, and re-sourcing replaces this
# one rather than stacking copies.
tmux set-hook -g 'window-linked[1]' "run-shell \"$DIR/scripts/ensure.sh '#{q:hook_window}'\""

# The picker opens in a popup, so it is bound to display-popup directly rather
# than going through run-shell.
picker_key="$(tmux show-option -gqv @agent_sidebar_picker_key)"
[ -z "$picker_key" ] && picker_key='g'
tmux bind-key "$picker_key" run-shell "$DIR/scripts/picker-open.sh"

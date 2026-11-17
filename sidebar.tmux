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

width=28

# if -F beats the next frame; the nested run covers a closed active pane, where the hook loses its window.
off="#{&&:#{==:#{pane_current_command},agent-sidebar},#{!=:#{pane_width},$width}}"
fix="#{P:#{?$off,resize-pane -t #{pane_id} -x $width ; ,}}"
tmux set-hook -g 'window-layout-changed[1]' "if -F -t '{left}' '$off' \"resize-pane -t '{left}' -x $width\" ; run -C \"run -C -t #{hook_window} '${fix//\#/##}'\""
tmux list-panes -a -F '#{pane_id} #{pane_current_command} #{pane_width}' |
	awk -v w="$width" '$2 == "agent-sidebar" && $3 != w { print $1 }' |
	while read -r p; do tmux resize-pane -t "$p" -x "$width"; done

# Refresh the shared pane map at once, not on its ten-second expiry.
for hook in session-created session-closed session-renamed; do
	tmux set-hook -g "$hook[1]" "run-shell -b \"'$DIR/agent-sidebar' --panes\""
done

# The picker opens in a popup sized from @agent_sidebar_picker_width/height.
picker_key="$(tmux show-option -gqv @agent_sidebar_picker_key)"
[ -z "$picker_key" ] && picker_key='g'
tmux bind-key "$picker_key" run-shell "$DIR/scripts/picker-open.sh"

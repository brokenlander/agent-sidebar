#!/bin/sh
# Claude Code SessionStart hook. Tags the tmux pane with the conversation id and
# the flags the agent was started with, so claude-panes.sh can bring it back
# after a tmux-resurrect restore. Silent, never fails the session.
[ -n "$TMUX_PANE" ] || exit 0
id=$(jq -r '.session_id // empty' 2>/dev/null)
[ -n "$id" ] || exit 0

# walk up to the process that spawned the hook chain
p=$PPID
while [ "$p" -gt 1 ] 2>/dev/null; do
    a0=$(tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | head -n 1)
    case "${a0##*/}" in claude) break ;; esac
    p=$(sed 's/.*) //' "/proc/$p/stat" 2>/dev/null | cut -d' ' -f2)
done

args=
[ "$p" -gt 1 ] 2>/dev/null && args=$("$(dirname "$0")/claude-panes.sh" args "$p")

tmux set -p -t "$TMUX_PANE" @claude_session "$id" 2>/dev/null
tmux set -p -t "$TMUX_PANE" @claude_args "$args" 2>/dev/null
exit 0

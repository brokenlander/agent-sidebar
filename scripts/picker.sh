#!/usr/bin/env sh
# Agent picker: a popup listing every agent, with a live preview of its screen.
# Enter jumps to one, ctrl-x kills one.
#
# The interaction design here - a popup fzf picker over the agent list, with a
# capture-pane preview and ctrl-x to kill - follows
# craftzdog/tmux-claude-session-manager by Takuya Matsuyama, MIT licensed.
# The implementation is independent: rows come from agent-sidebar's own loader,
# which reads the per-pid state files directly (about 6ms) rather than shelling
# out to the agent CLI (about 400ms).
set -u

DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/agent-sidebar"

[ -x "$BIN" ] || { tmux display-message "agent-sidebar: not built - run make in $DIR"; exit 0; }
for tool in fzf tmux; do
	command -v "$tool" >/dev/null 2>&1 || {
		tmux display-message "agent-sidebar: $tool is required for the picker"
		exit 0
	}
done

# Columns: 1 pid, 2 pane id, 3 state, 4 age, 5 session, 6 path.
# The first two are hidden; they are what the actions key on.
# A user's FZF_DEFAULT_OPTS is for interactive shell use and does not suit a
# popup: --height makes fzf occupy a fraction of it and leave the rest blank,
# and --border draws a second frame inside the popup's own. Start from nothing.
FZF_DEFAULT_OPTS=''
export FZF_DEFAULT_OPTS

# fzf's own input is a search box, so a bare "k" would type rather than kill.
# The actions use alt- mnemonics, with ctrl-x kept as an alias.
hdr=$(printf '\033[38;5;110menter\033[0m jump   \033[38;5;110malt-k\033[0m kill   \033[38;5;110malt-p\033[0m park   \033[38;5;110malt-n\033[0m new   \033[38;5;110malt-v\033[0m preview   \033[38;5;110mesc\033[0m close')

# Columns: 1 pid, 2 pane id, 3 state, 4 age, 5 session, 6 path.
# The first two are hidden; they are what the actions key on.
sel=$("$BIN" --list | fzf --ansi --delimiter='\t' --with-nth=3..6 \
	--reverse --cycle --no-sort --header-first --header="$hdr" \
	--expect=alt-n \
	--preview='tmux capture-pane -ept {2} 2>/dev/null || echo "(not in a pane)"' \
	--preview-window='right,55%,follow,border-left' \
	--bind='alt-v:toggle-preview' \
	--bind='ctrl-/:toggle-preview' \
	--bind="alt-k:execute-silent($BIN --kill {1})+reload(sleep 0.3; $BIN --list)" \
	--bind="ctrl-x:execute-silent($BIN --kill {1})+reload(sleep 0.3; $BIN --list)" \
	--bind="alt-p:execute-silent($BIN --park {1})+reload($BIN --list)" \
	--bind="ctrl-p:execute-silent($BIN --park {1})+reload($BIN --list)")

[ -n "$sel" ] || exit 0

key=$(printf '%s\n' "$sel" | sed -n '1p')
row=$(printf '%s\n' "$sel" | sed -n '2p')
[ -n "$row" ] || exit 0

pid=$(printf '%s' "$row" | cut -f1)
dir=$(printf '%s' "$row" | cut -f6)
kind=$(printf '%s' "$row" | cut -f7)

if [ "$key" = "alt-n" ]; then
	# Prompt rather than firing: starting an agent costs real money, and
	# the target directory comes from wherever the cursor happened to be.
	tmux command-prompt -p "new $kind agent in:" -I "$dir" \
		"run-shell \"$BIN --new '%%' $kind\""
	exit 0
fi

"$BIN" --jump "$pid"

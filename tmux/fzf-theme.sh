# Picker styling for the tmux popups (agent-pick, agent-reopen, sesh.sh),
# in the terminal's own palette so it follows its theme. Source this, then call
# fzf with only picker-specific flags. Pair with `display-popup -B` so fzf's
# rounded border is the single frame.
export FZF_DEFAULT_OPTS="--ansi --no-sort --layout=reverse --info=inline-right \
--border=rounded --border-label-pos=3 --padding=0,1 --margin=0 \
--preview-window=border-rounded --prompt='  ' --pointer='❯' --marker='❯' \
--separator='─' --scrollbar='│' \
--color=fg:-1,fg+:bright-white:bold,bg:-1,bg+:-1,hl:blue,hl+:cyan \
--color=border:bright-black,label:blue,preview-border:bright-black,gutter:-1 \
--color=prompt:cyan,pointer:magenta,marker:green,spinner:cyan,info:bright-black \
--color=header:bright-black"

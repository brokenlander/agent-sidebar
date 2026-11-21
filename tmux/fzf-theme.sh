# Sleek Tokyo Night styling shared by the tmux popup pickers (agent-pick,
# sesh.sh). Source this, then call fzf with only picker-specific flags
# (--border-label, --header, --preview, --bind). Pair with `display-popup -B`
# so fzf's own rounded border is the single frame.
export FZF_DEFAULT_OPTS="--ansi --no-sort --layout=reverse --info=inline-right \
--border=rounded --border-label-pos=3 --padding=0,1 --margin=0 \
--preview-window=border-rounded --prompt='  ' --pointer='❯' --marker='❯' \
--separator='─' --scrollbar='│' \
--color=fg:#a9b1d6,fg+:#ffffff,bg:-1,bg+:#283457,hl:#7aa2f7,hl+:#7dcfff \
--color=border:#3b4261,label:#7aa2f7,preview-border:#3b4261,gutter:-1 \
--color=prompt:#7dcfff,pointer:#bb9af7,marker:#9ece6a,spinner:#7dcfff,info:#565f89 \
--color=header:#565f89"

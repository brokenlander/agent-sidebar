#!/bin/sh
# Brings agent conversations back after a tmux-resurrect restore, for every
# agent in bin/agent-defs.
#   save      record every agent pane (resurrect post-save-all hook)
#   restore   relaunch each recorded conversation (post-restore-all hook)
#   revive    restart the board in any `board` window left on a bare shell
#   tag       refresh the panes' @claude_session and @claude_args
#   args PID  that process's flags, without the ones a resume sets itself
. "$HOME/forge/agent-sidebar/bin/agent-defs"
dir=${XDG_DATA_HOME:-$HOME/.local/share}/tmux/resurrect
f=$dir/claude-panes.tsv
tab=$TAB

# Flags only, plus the value of the few that take one: a positional prompt must
# not replay ahead of --resume, and --name is set again from the session name.
args_of() {
    tr '\n\0' ' \n' < "/proc/$1/cmdline" 2>/dev/null | tail -n +2 | awk '
        NR==1 && system("[ -f \"" $0 "\" ]")==0 { next }
        keep { keep=0; gsub(/\047/, "\047\\\047\047"); printf " \047%s\047", $0; next }
        skip { skip=0; if (!/^-/) next }
        $0=="-c" || $0=="--continue" || $0=="--fork-session" || /^--resume=/ || /^--name=/ { next }
        $0=="-r" || $0=="--resume" || $0=="-n" || $0=="--name" { skip=1; next }
        /^--(permission-mode|model|effort|settings|agent|fallback-model)$/ { keep=1 }
        /^-/ { printf "%s%s", (n++ ? " " : ""), $0 }'
}

# Every pane, tagged or not: the tag goes stale on resume and compact, the
# live process's session file does not.
tag_panes() {
    tmux list-panes -a -F "#{pane_id}${tab}#{pane_pid}${tab}#{pane_current_command}" |
    while IFS="$tab" read -r pane ppid cmd; do
        agent_valid "$cmd" || continue
        tmux set -p -t "$pane" @pm_agent_id "$cmd"
        [ "$cmd" = claude ] || continue
        c=$(agent_pid_of "$ppid" claude) || continue
        id=$(jq -r '.sessionId // .session_id // empty' "$HOME/.claude/sessions/$c.json" 2>/dev/null); [ -n "$id" ] || continue
        tmux set -p -t "$pane" @claude_session "$id"
        tmux set -p -t "$pane" @claude_args "$(args_of "$c")"
    done
}

revive_boards() {
    tmux list-panes -a -F "#{pane_id}${tab}#{window_name}${tab}#{pane_current_command}${tab}#{session_name}" |
    while IFS="$tab" read -r pane win cmd sess; do
        [ "$win" = board ] || continue
        case "$cmd" in sh | bash | zsh | fish) : ;; *) continue ;; esac
        tmux send-keys -t "$pane" "exec '$HOME/forge/agent-sidebar/bin/agent-board' watch" Enter
        printf 'revived board in %s\n' "$sess"
    done
}

case "$1" in
args) args_of "$2" ;;
tag) tag_panes ;;
revive) revive_boards ;;
save)
    mkdir -p "$dir"
    tag_panes
    # one list per resurrect save, named like it, so a restore reads the one for its layout
    ts=$(readlink "$dir/last" 2>/dev/null | sed -n 's/^tmux_resurrect_\(.*\)\.txt$/\1/p')
    out=$f; [ -n "$ts" ] && out="$dir/claude-panes_$ts.tsv"
    tmux list-panes -a -F "#{session_name}${tab}#{window_index}${tab}#{pane_index}${tab}#{pane_current_path}${tab}#{pane_current_command}${tab}#{@claude_session}${tab}#{@claude_args}" |
        awk -F'\t' -v OFS='\t' -v ags="$PM_AGENTS" '
            BEGIN{n=split(ags,a," ")}
            { ok=0; for(i=1;i<=n;i++) if($5==a[i]) ok=1
              if(!ok) next
              if($5=="claude" && $6=="") next
              print $1,$2,$3,$4,$5,$6,$7 }' > "$out.tmp" &&
        mv "$out.tmp" "$out" && { [ "$out" = "$f" ] || ln -sfn "$(basename "$out")" "$f"; }
    keep=$(tmux show-option -gqv @resurrect-delete-backup-after 2>/dev/null)
    case "$keep" in '' | *[!0-9]*) keep=30 ;; esac
    find "$dir" -maxdepth 1 -type f -name 'claude-panes_*.tsv' -mtime "+$keep" -delete 2>/dev/null
    ;;
restore)
    [ -s "$f" ] || exit 0
    sleep 2
    # resolve every target to a pane id first: killing a pane renumbers the rest
    launch=$(while IFS="$tab" read -r s w p path agent id args; do
        pid=$(tmux display -p -t "=$s:$w.$p" '#{pane_id}' 2>/dev/null) || continue
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$pid" "$path" "$agent" "$id" "$s" "$args"
    done < "$f")
    # resurrect brings the sidebar panes back as bare shells beside the ones the plugin reopens
    [ -r "$dir/last" ] && awk -F'\t' '$1=="pane" && $10=="agent-sidebar" {print $2 "\t" $3 "\t" $6}' "$dir/last" |
    while IFS="$tab" read -r s w p; do
        pid=$(tmux display -p -t "=$s:$w.$p" '#{pane_id}' 2>/dev/null) || continue
        case "$(tmux display -p -t "$pid" '#{pane_current_command}')" in *sh) tmux kill-pane -t "$pid" ;; esac
    done
    [ -n "$launch" ] && printf '%s\n' "$launch" | while IFS="$tab" read -r pid path agent id sess args; do
        [ -n "$pid" ] || continue
        tmux display -p -t "$pid" '' >/dev/null 2>&1 || continue
        agent_valid "$(tmux display -p -t "$pid" '#{pane_current_command}')" && continue
        cmd=$(agent_resume_cmd "$agent" "$id" "$args" "$sess") || continue
        tmux send-keys -t "$pid" "cd '$path' && $cmd" Enter
        sleep 1
    done
    revive_boards >/dev/null
    ;;
*)
    echo "usage: $0 save|restore|revive|tag|args PID" >&2; exit 2 ;;
esac

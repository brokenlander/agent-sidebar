#!/usr/bin/env bash
# Drives a real sidebar inside a real tmux server and asserts the behaviours
# where the interaction bugs have actually occurred: click parsing, client
# resolution, parking, renaming, overflow, and names containing spaces.
#
# Agents are synthesised: a `sleep` process is started in a pane and a session
# file is written for its pid, so the fixture is deterministic and the tests
# never depend on which agents happen to be running.
set -u

BIN="$(cd "$(dirname "$0")/.." && pwd)/agent-sidebar"
TMP="$(mktemp -d)"
SOCK="agent-sidebar-test-$$"
export XDG_STATE_HOME="$TMP/state"        # never touch the real park list
export CLAUDE_CONFIG_DIR="$TMP/claude"    # never read the real agents
mkdir -p "$CLAUDE_CONFIG_DIR/sessions" "$XDG_STATE_HOME"

TM="tmux -L $SOCK"
SOCKPATH="${TMUX_TMPDIR:-/tmp}/tmux-$(id -u)/$SOCK"
# Any invocation of the binary must talk to the TEST server. Without this its
# tmux calls go to the default socket - which once renamed a real session.
sb() { TMUX="$SOCKPATH,0,0" "$BIN" "$@"; }
fails=0
ok()   { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
check(){ if [ "$1" = 0 ]; then ok "$2"; else fail "$2"; fi; }

cleanup() { $TM kill-server 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

# --- fixture -----------------------------------------------------------------
# procStart is field 22 of /proc/<pid>/stat, counted after the last ')'
proc_start() { awk '{ n=split(substr($0, index($0,")")+2), f, " "); print f[20] }' "/proc/$1/stat"; }

make_agent() { # <session> <status>
	$TM new-session -d -s "$1" -x 100 -y 30 "sleep 600" 2>/dev/null
	local pid; pid=$($TM list-panes -t "$1" -F '#{pane_pid}' | head -1)
	cat > "$CLAUDE_CONFIG_DIR/sessions/$pid.json" <<EOF
{"pid":$pid,"procStart":"$(proc_start "$pid")","status":"$2","cwd":"$TMP/work",
 "name":"$1","kind":"interactive"}
EOF
}

click() { # <pane> <button> <row>
	local hex="1b 5b 3c $(printf '%s' "$2" | od -An -tx1 | tr -d ' \n') 3b 35 3b"
	for c in $(printf '%s' "$3" | fold -w1); do hex="$hex $(printf '%s' "$c" | od -An -tx1 | tr -d ' \n')"; done
	$TM send-keys -t "$1" -H $hex 4d
}

# --- setup -------------------------------------------------------------------
$TM new-session -d -s host -x 120 -y 30
make_agent alpha idle
make_agent bravo busy
make_agent "with space" idle
$TM new-session -d -s bar -x 120 -y 30
$TM split-window -h -b -l 30 -d -t bar -e AGENT_SIDEBAR_TRACE="$TMP/trace" "$BIN"
sleep 2
PANE=$($TM list-panes -t bar -F '#{pane_id} #{pane_current_command}' | awk '$2=="agent-sidebar"{print $1}')
view() { $TM capture-pane -p -t "$PANE" | sed 's/\x1b\[[0-9;]*m//g'; }
row_of() { view | grep -n "$1" | head -1 | cut -d: -f1; }

echo "integration tests"

# --- render ------------------------------------------------------------------
view | grep -q "alpha" && check 0 "renders a live agent" || check 1 "renders a live agent"
view | grep -q "with space" && check 0 "renders a name containing spaces" || check 1 "renders a name containing spaces"
[ "$(view | grep -c '●')" -ge 3 ] && check 0 "renders one row per agent" || check 1 "renders one row per agent"

# --- liveness ----------------------------------------------------------------
echo '{"pid":999999,"procStart":"1","status":"idle","cwd":"/x","name":"ghost","kind":"interactive"}' \
	> "$CLAUDE_CONFIG_DIR/sessions/999999.json"
sleep 2
view | grep -q "ghost" && check 1 "a dead pid is not shown" || check 0 "a dead pid is not shown"
rm -f "$CLAUDE_CONFIG_DIR/sessions/999999.json"

# --- parking -----------------------------------------------------------------
r=$(row_of "alpha"); click "$PANE" 2 "$r"; sleep 2
grep -qx "alpha" "$XDG_STATE_HOME/agent-sidebar/parked" 2>/dev/null \
	&& check 0 "right-click parks" || check 1 "right-click parks"
[ "$(view | grep -n 'alpha' | cut -d: -f1)" -gt "$r" ] \
	&& check 0 "a parked agent sinks down the list" || check 1 "a parked agent sinks down the list"
click "$PANE" 2 "$(row_of 'alpha')"; sleep 2
grep -qx "alpha" "$XDG_STATE_HOME/agent-sidebar/parked" 2>/dev/null \
	&& check 1 "right-click again un-parks" || check 0 "right-click again un-parks"

# --- split input -------------------------------------------------------------
before=$(grep -c "click" "$TMP/trace" 2>/dev/null || echo 0)
$TM send-keys -t "$PANE" -H 1b 5b 3c 32 3b 35; sleep 1
$TM send-keys -t "$PANE" -H 3b 34 4d; sleep 2
[ "$(grep -c 'click' "$TMP/trace" 2>/dev/null || echo 0)" -gt "$before" ] \
	&& check 0 "a click split across reads still registers" || check 1 "a click split across reads still registers"
click "$PANE" 2 "$(row_of 'alpha')" >/dev/null 2>&1; sleep 1   # undo that park

# --- rename ------------------------------------------------------------------
p=$($TM list-panes -t alpha -F '#{pane_pid}' | head -1)
sb --rename-session "$p" "alpha renamed" >/dev/null 2>&1
sleep 2
$TM list-sessions -F '#{session_name}' | grep -qx "alpha renamed" \
	&& check 0 "rename accepts a name with spaces" || check 1 "rename accepts a name with spaces"

# a pid that matches no agent must be refused, never guessed at as a target
sb --rename-session 999999 "must not happen" >/dev/null 2>&1
[ $? -eq 2 ] && check 0 "rename refuses an unknown pid" || check 1 "rename refuses an unknown pid"
$TM list-sessions -F '#{session_name}' | grep -q "must not happen" \
	&& check 1 "a refused rename changes nothing" || check 0 "a refused rename changes nothing"

# --- picker feed -------------------------------------------------------------
rows=$(sb --list | wc -l)
[ "$rows" -ge 3 ] && check 0 "--list emits a row per agent" || check 1 "--list emits a row per agent"
sb --list | head -1 | awk -F'\t' '{ exit !($1 ~ /^[0-9]+$/ && $2 ~ /^%/) }' \
	&& check 0 "--list hides pid and pane id in the first two columns" \
	|| check 1 "--list hides pid and pane id in the first two columns"

# --- kill --------------------------------------------------------------------
victim=$($TM list-panes -t bravo -F '#{pane_pid}' | head -1)
[ -n "$victim" ] || fail "could not find a victim pid"   # an empty pid would pass vacuously
sb --kill "$victim" >/dev/null 2>&1
# poll rather than sleeping a fixed amount: under load the exit is not instant,
# and a fixed wait made this check flaky
# kill -0 succeeds on a zombie, so it is not a liveness test: the agent exits
# but lingers unreaped until tmux gets round to it, which made this flaky.
dead() {
	[ -r "/proc/$1/stat" ] || return 0
	[ "$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null)" = "Z" ]
}
gone=1
for _ in 1 2 3 4 5 6 7 8 9 10; do
	dead "$victim" && { gone=0; break; }
	sleep 0.5
done
check "$gone" "--kill terminates the agent"

sb --list | awk -F'\t' -v v="$victim" '$1==v{found=1} END{exit !found}' \
	&& check 1 "a killed agent leaves the list" || check 0 "a killed agent leaves the list"

sb --kill 999999 >/dev/null 2>&1
[ $? -eq 2 ] && check 0 "--kill refuses an unknown pid" || check 1 "--kill refuses an unknown pid"

sleep 300 & bystander=$!
sleep 0.3
sb --kill "$bystander" >/dev/null 2>&1
kill -0 "$bystander" 2>/dev/null \
	&& check 0 "--kill will not signal a non-agent" || check 1 "--kill will not signal a non-agent"
kill "$bystander" 2>/dev/null

# --- new ---------------------------------------------------------------------
$TM set -g @agent_sidebar_new_command "sleep 120"
mkdir -p "$TMP/fresh"
sb --new "$TMP/fresh" >/dev/null 2>&1
sleep 1
$TM list-sessions -F '#{session_name}' | grep -qx "fresh" \
	&& check 0 "--new creates a session named for the directory" \
	|| check 1 "--new creates a session named for the directory"
[ "$($TM display-message -p -t fresh '#{pane_current_path}' 2>/dev/null)" = "$TMP/fresh" ] \
	&& check 0 "--new starts it in that directory" || check 1 "--new starts it in that directory"
sb --new "$TMP/fresh" >/dev/null 2>&1
sleep 1
$TM list-sessions -F '#{session_name}' | grep -qx "fresh-2" \
	&& check 0 "--new suffixes rather than colliding" || check 1 "--new suffixes rather than colliding"
sb --new "$TMP/not-a-dir" >/dev/null 2>&1
[ $? -eq 2 ] && check 0 "--new refuses a non-directory" || check 1 "--new refuses a non-directory"

# --- width -------------------------------------------------------------------
# tmux hands a sidebar half of any change in window width; it must put itself
# back, unless the border itself was dragged. tmux passes a pane's new size to
# the application at most once a second, so each step is waited for, not slept
# through: a fixed sleep landed on that boundary and lost the drag.
width_is() { # <cols>
	for _ in {1..20}; do
		[ "$($TM display-message -p -t "$PANE" '#{pane_width}')" = "$1" ] && return 0
		sleep 0.25
	done
	return 1
}
traced() { # <text>
	for _ in {1..20}; do
		grep -q "$1" "$TMP/trace" 2>/dev/null && return 0
		sleep 0.25
	done
	return 1
}
rows0=$(view | grep -c .)
$TM resize-window -t bar -x 180
width_is 30 && check 0 "holds its width when the window grows" \
	|| check 1 "holds its width when the window grows"
$TM resize-pane -t "$PANE" -x 40                    # a drag of the border
traced "dragged 30 -> 40" && check 0 "a border drag is not undone" \
	|| check 1 "a border drag is not undone"
$TM resize-window -t bar -x 120
width_is 40 && check 0 "a dragged width is the one held" \
	|| check 1 "a dragged width is the one held"

# --- fresh -------------------------------------------------------------------
# tmux reflows a pane's text when its width changes, and once left a wrapped
# tail of the old frame below the new one. The sidebar runs on the alternate
# screen, which tmux never reflows, and a repaint after a resize clears first.
sleep 2
[ "$(view | grep -c .)" = "$rows0" ] && check 0 "a resize leaves no stale rows behind" \
	|| check 1 "a resize leaves no stale rows behind"

# --- overflow ----------------------------------------------------------------
sb --once --width 26 --rows 4 | grep -q "more" \
	&& check 0 "a short pane reports hidden rows" || check 1 "a short pane reports hidden rows"
sb --once --width 26 --rows 40 | grep -q "more" \
	&& check 1 "a tall pane reports nothing hidden" || check 0 "a tall pane reports nothing hidden"

# --- one control -------------------------------------------------------------
# prefix+e is one control for every session: open everywhere, close everywhere,
# and a window made later comes up the same way through the window-linked hook
# that sidebar.tmux installs. Last, because it touches every window.
ROOT="$(dirname "$BIN")"
tm_env() { TMUX="$SOCKPATH,0,0" "$@"; }
tm_env bash "$ROOT/sidebar.tmux"
windows()      { $TM list-windows -a -F '#{window_id}' | wc -l; }
with_sidebar() { $TM list-panes -a -F '#{window_id} #{pane_current_command}' | awk '$2=="agent-sidebar"{print $1}' | sort -u | wc -l; }
$TM new-session -d -s lone -x 120 -y 30 "$BIN"     # a window with nothing but a sidebar
sleep 1
host_win=$($TM display-message -p -t host '#{window_id}')
tm_env "$ROOT/scripts/toggle.sh" "$host_win"; sleep 2
[ "$(with_sidebar)" = "$(windows)" ] \
	&& check 0 "toggling in a bare window opens one in every window" \
	|| check 1 "toggling in a bare window opens one in every window"
[ "$($TM list-panes -t lone | wc -l)" = 1 ] \
	&& check 0 "a window that already has one is left alone" \
	|| check 1 "a window that already has one is left alone"
$TM new-session -d -s later -x 120 -y 30 "sleep 600"; sleep 2
$TM list-panes -t later -F '#{pane_current_command}' | grep -qx agent-sidebar \
	&& check 0 "a session created later gets one" \
	|| check 1 "a session created later gets one"
bar_win=$($TM display-message -p -t bar '#{window_id}')
tm_env "$ROOT/scripts/toggle.sh" "$bar_win"; sleep 2
[ "$(with_sidebar)" = 1 ] \
	&& check 0 "toggling in a window with one closes them all" \
	|| check 1 "toggling in a window with one closes them all"
$TM has-session -t lone 2>/dev/null \
	&& check 0 "a window with nothing but a sidebar is not closed" \
	|| check 1 "a window with nothing but a sidebar is not closed"
$TM new-session -d -s after -x 120 -y 30 "sleep 600"; sleep 2
$TM list-panes -t after -F '#{pane_current_command}' | grep -qx agent-sidebar \
	&& check 1 "a session created after a close stays bare" \
	|| check 0 "a session created after a close stays bare"

echo
[ "$fails" -eq 0 ] && { echo "  OK    all integration checks passed"; exit 0; }
echo "  $fails check(s) failed"; exit 1

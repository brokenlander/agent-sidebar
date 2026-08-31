# claude-sidebar

A tmux sidebar showing every Claude Code agent on the machine: what state it is
in, which tmux session it lives in, and how long it has been that way.

```
 claude                   17
 ○ 1  ● 11  ● 5
──────────────────────────────
 ● PR-4 gateway-api       2m
 ● summit kg-summit       4d
 ● csi gateway-api       11m
 ○ price gpu-calculator    4d   <- parked
```

Amber needs you, green is idle and yours, red is working. An agent that has
backgrounded a shell command still counts as idle - it is not a state you act
on differently. Sorted so whatever needs you is at the top.

## Clicking

- **Left-click a row** to jump to that agent: the client looking at the sidebar
  switches session, window and pane in one go.
- **Middle-click a row** for a menu: jump, park/un-park, or rename the tmux
  session. tmux renders the menu itself via `display-menu`, so there is no menu
  widget here - just a command string. Rename pre-fills the current name and
  moves the park entry with it, so a parked agent does not un-park because its
  key changed.
- **Right-click a row** to park it directly, without the menu. Parked agents sink below
  Parked agents sink below everything else, whatever they are doing, and render
  as a hollow dimmed dot. Click again to bring one back.

Parking is per tmux session name and survives restarts, in
`$XDG_STATE_HOME/claude-sidebar/parked` (default `~/.local/state/...`). It is a
plain list of session names, so it is editable by hand.

Right-click only reaches the sidebar if your config forwards it. Many configs
bind `MouseDown3Pane` unconditionally, and a binding takes precedence over
forwarding the event to the pane's application. The fix is to forward when the
pane has asked for mouse events:

```tmux
bind -n MouseDown3Pane if -F "#{||:#{pane_in_mode},#{mouse_any_flag}}" \
  { send -M } { select-pane -t= ; run-shell '/path/to/your-paste-script' }
```

Middle-click works either way, since tmux's default `MouseDown2Pane` binding
already forwards on that condition.

## Why it is cheap

The obvious way to build this is to poll `claude agents --json` and repaint on a
timer. Both halves are expensive:

- `claude agents --json` costs **415 ms of CPU** per call on a 16-agent machine.
  Polling it once a second spends 40% of a core.
- Repainting unconditionally makes the tmux server re-render the pane for every
  attached client. The tmux server is single-threaded, so that cost is paid
  serially against everything else you do in tmux.

This reads `~/.claude/sessions/*.json` instead, which every agent writes for
itself. The same 16 agents come back in **3-4 ms**, ~120x cheaper, and the data
is richer: `status`, `cwd`, `statusUpdatedAt`, and often the tmux pane already
resolved.

On top of that:

- **inotify, not polling.** A status change lands within a millisecond of the
  agent writing it. The 1s timer exists only so the age column stays honest.
- **Line-level diffing.** Each refresh builds a frame and writes only the lines
  that differ. An unchanged sidebar sends the tmux server zero bytes.
- **Pane resolution is cached per PID.** The `tmux` field is missing from about
  half the session files, so the pane is resolved from the process's controlling
  tty. That answer is cached, including negative results, so `tmux list-panes`
  runs once per new agent rather than once per refresh.

Measured over 30 seconds against 16 live agents: **0% CPU for the sidebar, ~1%
for the tmux server.**

## Debugging

```sh
./claude-sidebar --debug                 # dump resolved agent rows and exit
CLAUDE_SIDEBAR_TRACE=/tmp/cs.log ./claude-sidebar   # log input and jump commands
```

The trace costs one `getenv` per input event and nothing at all when unset.

## Build

```sh
make          # -> ./claude-sidebar
make test     # ASan + UBSan, fixtures plus every real session file on the box
```

No dependencies beyond libc.

## Install

With TPM, in `~/.tmux.conf`:

```tmux
set -g @plugin 'tmux-claude-sidebar'
set -g @claude_sidebar_key 'e'     # prefix + e toggles
set -g @claude_sidebar_width '34'
```

Or run it in any pane directly:

```sh
./claude-sidebar            # live
./claude-sidebar --once     # print one frame and exit
```

## What it does not do

No keyboard handling and no killing agents. Mouse input costs nothing - the same
`poll` that waits on inotify waits on stdin - but a keyboard-driven TUI implies
selection state, cursor movement and a repaint loop, which is the cost this
design exists to avoid.

## Caveats

`~/.claude/sessions/` is an internal Claude Code layout, not a documented API. It
can change between versions. The parser skips anything it does not recognise
rather than failing, so the worst case is a row going missing, not a crash.

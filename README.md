# claude-sidebar

A tmux sidebar showing every Claude Code agent on the machine: what state it is
in, which tmux session it lives in, and how long it has been that way.

```
 claude                   16
 ● 12  ● 1  ● 3
──────────────────────────────
 ● PR-4 gateway-api       2m
 ● summit kg-summit       4d
 ● csi gateway-api       11m
```

Amber needs you, green is idle and yours, blue dropped to a shell, red is
working. Sorted so whatever needs you is at the top.

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

It is a display, not a controller — no keyboard handling, no jumping, no killing.
That is deliberate: making the pane interactive is what turns it into a TUI with
a repaint loop, which is the cost this design exists to avoid. Pair it with a
picker bound to its own key for jumping.

## Caveats

`~/.claude/sessions/` is an internal Claude Code layout, not a documented API. It
can change between versions. The parser skips anything it does not recognise
rather than failing, so the worst case is a row going missing, not a crash.

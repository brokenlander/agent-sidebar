# agent-sidebar

A tmux sidebar showing every coding agent on the machine: what state it is in,
which tmux session it lives in, and how long it has been that way. Click a row
to jump straight to it.

Supports Claude Code natively and opencode through a small plugin. Written in
C17 against libc only; a refresh costs about 4ms and an idle sidebar writes
nothing at all.

```
 agents                   17
 ○ 1  ● 11  ● 5
──────────────────────────────
 ● api gateway-api        2m
 ● docs handbook          4d
 ● web storefront        11m
 ○ price gpu-calculator    4d   <- parked
```

Amber needs you, green is idle and yours, red is working. An agent that has
backgrounded a shell command still counts as idle - it is not a state you act
on differently. Sorted so whatever needs you is at the top.

## One control

`prefix + e` is one switch for every session. Pressed in a window without a
sidebar it opens one in every window of every session; pressed in a window
with one it closes them all. A window or session created while they are open
gets one as it appears, through a `window-linked` hook that `sidebar.tmux`
installs at index 1 of that hook, so one you set yourself at index 0 is left
alone. A window holding nothing but a sidebar is never closed, since that
would close the window and its session with it.

Set `@agent_sidebar_exclude` to a session-name pattern (a shell `case` glob,
e.g. `scratch` or `scratch|popup*`) to keep those sessions sidebar-free - a
scratch popup wants a bare terminal, not the panel.

## Clicking

- **Left-click a row** to jump to that agent: the client looking at the sidebar
  switches session, window and pane in one go.
- **Middle-click a row** for a menu: jump, park/un-park, rename the tmux
  session, start another agent, or kill one. tmux renders the menu itself via
  `display-menu`, so there is no menu widget here - just a command string.
  Choose an item with the mouse or its bracketed key. Rename pre-fills the
  current name and moves the park entry with it, so a parked agent does not
  un-park because its key changed.

  The menu is passed `-M -O`. A menu opened by a program rather than by a tmux
  mouse binding is marked "no mouse", and the release of the very button that
  opened it would then close it again - the menu appeared to flash and vanish
  as you let go. `-M` (tmux 3.5+) lets the menu take the click, and `-O` keeps
  it up when a click lands off an item. On tmux older than 3.5, `-M` does not
  exist, so the menu is keyboard-only and still closes on the release; its keys
  work, but a mouse choice needs 3.5 or newer.
- **Right-click a row** to park it directly, without the menu. Parked agents
  sink below everything else, whatever they are doing, and render as a hollow
  dimmed dot. Click again to bring one back.

Parking is per tmux session name and survives restarts, in
`$XDG_STATE_HOME/agent-sidebar/parked` (default `~/.local/state/...`). It is a
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

## Width

The sidebar opens at `@agent_sidebar_width` columns (default 28) and holds
that width. tmux shares a change in window width equally between side-by-side
panes, so a window last sized by a narrower terminal hands the sidebar half of
the difference when a wider one looks at it: 28 columns came back as 61, and
from 40 the rows gain a directory column. When the window's width changes the
sidebar puts itself back. A resize with the window unchanged is you dragging
the border, and that width is the one held from then on.

The pane runs on the alternate screen, which tmux never reflows, and a repaint
after a resize clears the pane first. Before that, narrowing a pane wrapped
every row and the wrapped tails sat below the new frame as a stale copy of it,
which is what an old session's sidebar looked like until it was reopened.

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
./agent-sidebar --debug                 # dump resolved agent rows and exit
AGENT_SIDEBAR_TRACE=/tmp/cs.log ./agent-sidebar   # log input and jump commands
```

The trace costs one `getenv` per input event and nothing at all when unset.

## The picker

`prefix + g` opens a popup listing every agent, with a live preview of its
screen.

| key | |
|---|---|
| `enter` | jump to it |
| `alt-k` | kill it |
| `alt-p` | park it |
| `alt-n` | start another agent of the same kind, in its directory (prompts) |
| `alt-v` | toggle the preview |
| typing | filters the list |

`alt-` rather than bare letters because fzf's input is a search box, and with a
fleet of agents that filter earns its keep. The `ctrl-x` and `ctrl-p` forms work
too.

`alt-n` starts whatever the highlighted row is running - an opencode row starts
opencode - and prompts with that row's directory first, since starting an agent
costs real money and the directory depends on where the cursor was. The command
per kind is `@agent_sidebar_command_claude`, `@agent_sidebar_command_opencode`
and so on; unset, the kind is the command.

The sidebar answers "what is happening"; the picker answers "show me that one"
without giving up a column of screen. Both read the same loader, so opening the
picker costs about 6ms.

## Requirements

tmux 3.2 or newer, a C compiler, and libc. Node is needed only to run the
opencode producer and its tests.

## Quick start

```sh
make
./agent-sidebar --once        # print one frame and exit, to see it working
```

Then load it from `~/.tmux.conf`:

```tmux
set -g @agent_sidebar_key 'e'         # prefix + e toggles the sidebar everywhere
set -g @agent_sidebar_width '28'
set -g @agent_sidebar_picker_key 'g'  # prefix + g opens the picker
run-shell -b '/path/to/agent-sidebar/sidebar.tmux'
```

Claude Code needs no setup - it already writes the state this reads. For
opencode, symlink the producer into its plugin directory:

```sh
mkdir -p ~/.config/opencode/plugins
ln -sf "$PWD/producers/opencode/plugin.js" ~/.config/opencode/plugins/agent-sidebar.js
```

Restart an agent afterwards; it registers on startup and removes itself on exit.

## Build

```sh
make          # -> ./agent-sidebar
make test     # ASan + UBSan, fixtures plus every real session file on the box
```

No dependencies beyond libc.

## Install

With TPM, in `~/.tmux.conf`:

```tmux
set -g @plugin 'agent-sidebar'
set -g @agent_sidebar_key 'e'     # prefix + e toggles
set -g @agent_sidebar_width '34'
```

Or run it in any pane directly:

```sh
./agent-sidebar            # live
./agent-sidebar --once     # print one frame and exit
```

## Credit

The picker's interaction design - a popup fzf list over the running agents,
with a `capture-pane` preview and `ctrl-x` to kill - follows
[craftzdog/tmux-claude-session-manager](https://github.com/craftzdog/tmux-claude-session-manager)
by Takuya Matsuyama, MIT licensed. The implementation here is independent: rows
come from the per-pid state files rather than the agent CLI, which is the
difference between roughly 6ms and 400ms, and it covers opencode as well.

## What it does not do

No keyboard handling and no killing agents. Mouse input costs nothing - the same
`poll` that waits on inotify waits on stdin - but a keyboard-driven TUI implies
selection state, cursor movement and a repaint loop, which is the cost this
design exists to avoid.

## Caveats

`~/.claude/sessions/` is an internal Claude Code layout, not a documented API. It
can change between versions. The parser skips anything it does not recognise
rather than failing, so the worst case is a row going missing, not a crash.

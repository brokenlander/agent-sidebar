# agent-sidebar

A tmux sidebar showing every coding agent on the machine: what state it is in,
which tmux session it lives in, and how long it has been that way. Click a row
to jump straight to it.

Supports Claude Code natively and opencode through a small plugin. Written in
C17 against libc only; a refresh costs about 4ms and an idle sidebar writes
nothing at all.

```
 agents                   17
 ○ 1  ● 2  ● 9  ● 5
──────────────────────────────
 ● api gateway-api        2m
 ● web storefront        11m
 ● docs handbook          4d
 ● infra runbooks         3h   <- frozen
 ○ price gpu-calculator    4d   <- parked
```

Purple is blocked on you, amber is one you forgot, green is idle and yours,
red is working. An agent that has backgrounded a shell command still counts as
idle - it is not a state you act on differently. Rows run newest change first,
whatever their colour, so the one you were just talking to is at the top.
Frozen agents sit below the live ones in deep blue; parked ones sink to the
bottom.

**Purple is reported, amber is inferred, and they are not the same thing.**
Purple means the agent said `waiting` itself: something is up on its screen and
nothing moves until you answer it - a permission prompt, or any dialog it is
sitting in. Amber means nobody is blocking it and you simply stopped coming
back: idle past `@agent_sidebar_idle_wait` minutes (default 30, `0` disables)
is promoted to amber. That is a display rule over
`statusUpdatedAt`, which the age column already shows - no hook, no extra state
file, and it applies to sessions that are already running. Parked and frozen
agents are exempt: set aside is set aside.

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
- **Middle-click a row** for a menu: jump, freeze/un-freeze, park/un-park,
  rename the tmux session, start another agent, or kill one. Rename starts
  from an EMPTY field — the old name is in the prompt, not in your way — and
  an empty answer is refused rather than acted on. tmux renders the menu
  itself via `display-menu`, so there is no menu widget here - just a command
  string. Choose an item with the mouse or its bracketed key. Rename moves the
  park and freeze entries with it, so a listed agent does not drop off because
  its key changed.

  The menu is passed `-M -O`. A menu opened by a program rather than by a tmux
  mouse binding is marked "no mouse", and the release of the very button that
  opened it would then close it again - the menu appeared to flash and vanish
  as you let go. `-M` (tmux 3.5+) lets the menu take the click, and `-O` keeps
  it up when a click lands off an item. On tmux older than 3.5, `-M` does not
  exist, so the menu is keyboard-only and still closes on the release; its keys
  work, but a mouse choice needs 3.5 or newer.
- **Session rows** (`@agent_sidebar_sessions on` lists the sessions that hold
  no agent) jump on a left-click; a middle-click offers jump or kill.
- **Right-click a row** to park it directly, without the menu. Parked agents
  sink below everything else, whatever they are doing, and render as a hollow
  dimmed dot. Click again to bring one back.
- **Click `? shortcuts`**, the sidebar's last row, for a menu of your tmux
  shortcuts. They come from `@agent_sidebar_legend`, as `key label` entries
  separated by `;` so a key can be `|` or `-`, e.g.
  `'e sidebar;| split right;- split down'`. tmux draws each key on the right.
  A word ending in `:` starts a titled section, so
  `'agents:;a new agent;q end agent;tmux:;| split right'` shows two groups.
  The menu only shows them; choosing an item does nothing. Leave the option
  unset and there is no button.
- **Freeze** (`f` on the menu, `alt-f` in the picker) sets an agent aside for
  later: the row turns deep blue and drops below the live ones, above the
  parked. The agent stays open and keeps whatever it was doing; freeze again
  to bring it back. Park is for what you are done with, freeze for what you
  are coming back to.

A session name can hold anything tmux allows: spaces, quotes, `;`, `.`, `#`.
Jumps and menu commands target the pane or session id, never the name, since
tmux reads `v1.2` as session `v1`, pane 2; a name that only appears in a prompt
or title is quoted, and `#` doubled, since those are formats. What you type at
a prompt (a new name, a directory) goes into `@agent_sidebar_input` as typed,
and the binary reads it from there, so no shell sees it.

Parking and freezing are per tmux session name and survive restarts, in
`$XDG_STATE_HOME/agent-sidebar/parked` and `.../frozen` (default
`~/.local/state/...`). Each is a plain list of session names, so it is
editable by hand.

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

Every sidebar is 28 columns, in every window, always, and there is no option to
change it. tmux shares any change in a window's width among the panes in it, so
the sidebar gets a slice of everything: a wider terminal looking at the window
(28 came back as 53), closing the pane beside it (all of that pane's width, 152
columns), a new layout. `sidebar.tmux` installs a `window-layout-changed` hook
that gives the sidebar its width back inside tmux, before the frame is drawn.
Dragging the border does nothing. Loading `sidebar.tmux` also puts every open
sidebar back to 28.

The sidebar used to hold its own width: on `SIGWINCH` it asked tmux for the
window's width and resized itself back. That left the wrong width on screen for
30 to 40 ms on every change, and up to a second when tmux held back the signal.
It could also not tell a drag from a resize except by comparing two numbers that
were read at different moments, so some sidebars settled at 31 to 38.

The hook puts it back twice over. `if -F` on the window's leftmost pane runs in
the same pass as the change, so tmux draws the right width first; a `run -C`
waits for a timer and left the wrong frame up for 10 to 20 ms. But when the
change is closing the window's active pane, tmux loses the hook's target and
runs it against whichever session it thinks is current. `hook_window` still
names the right window, so a nested `run -C` makes it the target, a pass later.

tmux still tells the program in the pane every size it passed through: about
10 ms before the 28 that puts it back, it hears 29 during a drag, 48 when the
window grows and 1 when it shrinks. The sidebar laid its rows out for each, so
the contents jumped while the border stood still. On a resize it now asks tmux
for the pane's width instead, which the hook has already put back.

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

There is one sidebar per window, so whatever one sidebar does, a hundred do.
On top of that:

- **inotify, not polling.** A status change lands within a millisecond of the
  agent writing it. The 1s timer only redraws, so the age column stays honest;
  the files are read on a change, and every 5 seconds to re-check that each
  agent's process is still alive, since a crashed one leaves its file behind.
- **Only our files wake it.** The state directory is shared (pm-board writes
  there too), and the pane map's lock and temp files live beside the map.
  Waking on all of them once made every sidebar reload in step, so they all
  found the map stale at the same moment, queued on its lock, and each lock
  released woke every sidebar again.
- **A file is parsed when it changes.** A load lists the directories and stats
  each file; only one whose inode, size or mtime moved is read and parsed
  again.
- **One pane map for every sidebar.** The `tmux` field is missing from about
  half the session files, so an agent's pane comes from its controlling tty
  and `tmux list-panes -a`, which costs the single-threaded tmux server 3 to
  5 ms. Each sidebar used to run it for itself, every 5 seconds, on every
  status change, and every second while any agent was outside tmux. The map
  is now one file per tmux server in the state directory: a sidebar that finds
  it stale refreshes it under a lock, and the others wait, find it fresh and
  read it. It goes stale after 10 seconds, or when an agent started after it
  was fetched (one that started earlier and is still missing lives outside
  this server, and asking again would not find it). `sidebar.tmux` refreshes it
  from the `session-created`, `session-closed` and `session-renamed` hooks, so
  those show at once.
- **Line-level diffing.** Each refresh builds a frame and writes only the lines
  that differ. An unchanged sidebar sends the tmux server zero bytes.

Measured on 96 live sidebars watching 36 agents, before and after: the
sidebars went from 140 ms of CPU a second to 18, the tmux calls they made from
43 ms a second to none, and the tmux server, which does everything else too,
from 226 to about 60. One sidebar alone made 29 tmux calls in 30 idle seconds;
now one call every 10 seconds or so serves them all.

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
| `alt-f` | freeze it |
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
without giving up a column of screen. Both read the same loader and the same
pane map, so the list costs one tmux call for its options.

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

## Options

| option | default | |
|---|---|---|
| `@agent_sidebar_key` | `e` | `prefix + key` opens or closes every sidebar |
| `@agent_sidebar_picker_key` | `g` | `prefix + key` opens the picker |
| `@agent_sidebar_picker_width`, `_height` | `95%` | the picker popup's size |
| `@agent_sidebar_legend` | unset | `key label;...` for the `? shortcuts` menu; `title:` starts a section |
| `@agent_sidebar_sessions` | off | `on` lists sessions with no agent |
| `@agent_sidebar_exclude` | unset | sessions that never get a sidebar, `a\|b*` |
| `@agent_sidebar_idle_wait` | `30` | minutes idle before amber; `0` disables |
| `@agent_sidebar_command_<kind>` | the kind | what "New agent" runs for that kind |

A running sidebar reads its options once, at start.

## Install

With TPM, in `~/.tmux.conf`:

```tmux
set -g @plugin 'agent-sidebar'
set -g @agent_sidebar_key 'e'     # prefix + e toggles
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
difference between roughly 7ms and 400ms, and it covers opencode as well.

## What it does not do

No keyboard handling. Mouse input costs nothing - the same
`poll` that waits on inotify waits on stdin - but a keyboard-driven TUI implies
selection state, cursor movement and a repaint loop, which is the cost this
design exists to avoid.

## Caveats

`~/.claude/sessions/` is an internal Claude Code layout, not a documented API. It
can change between versions. The parser skips anything it does not recognise
rather than failing, so the worst case is a row going missing, not a crash.

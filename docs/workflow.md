# tmux-agent

How this tmux and coding-agent setup works: launching agents, watching
them, ending and resetting them, and bringing them back after a tmux restart.

| Piece | Location | Repo |
|---|---|---|
| sidebar (C), agent tools, tmux agent layer | `~/forge/agent-sidebar/` (`bin/`, `tmux/`) | `brokenlander/agent-sidebar` (public) |
| base tmux config (loads the agent layer) | `~/dotfiles/tmux/.tmux.conf` | `brokenlander/dotfiles` (public) |
| optional workspace with slots | `~/.config/agent-sidebar/workspace` | none, per machine |
| Claude settings, theme, statusline, persona | `~/.claude/` | none, local only |

Commits to both repos go out as
`brokenlander <94259413+brokenlander@users.noreply.github.com>`. No AI
co-author trailer. `git commit --no-verify` when file content has to contain
`claude` (the CLI name): the commit guard matches the word.

## Keys

tmux 3.7c, `/usr/local/bin/tmux`. The prefix is the default, `C-b`.

The `? shortcuts` row in the sidebar shows the same keys in two groups.

**herd**, the agent keys:

| Key | Action |
|---|---|
| `prefix a` | new agent (`agent-pick`) |
| `prefix u` | agent picker |
| `prefix e` | sidebar on or off, in every session at once |
| `prefix b` | this session's board window; again goes back |
| `prefix o` | reopen a closed conversation (`agent-reopen`) |
| `prefix B` | branch this conversation into its own session (`agent-branch`) |
| `prefix r` | reset this agent (`agent --reset`) |
| `prefix q` | end this agent and reset its slot, no prompt (`agent-end`) |

**tmux**:

| Key | Action |
|---|---|
| `prefix y` | sesh session jumper |
| `prefix s` | choose-tree |
| `prefix d` | scratch popup; `prefix D` detaches |
| `prefix \|`, `prefix -` | split right, split down; `prefix x` closes a pane |
| `prefix R` | reload the tmux config |

Pickers share `tmux/fzf-theme.sh` and open with `display-popup -B`. The status
bar forks nothing: no `#()` anywhere in a format.

## Sidebar

`agent-sidebar` is an inotify-driven C program shown as a 28-column pane in
every window by the `sidebar.tmux` hook. It lists agents, then the other
sessions, then one `? shortcuts` row that opens `@agent_sidebar_legend` as a
menu. Its README has the detail. What matters from here:

- Options are in `.tmux.conf` (`@agent_sidebar_key`, `_sessions`, `_exclude`,
  `_picker_key`, `_idle_wait`, `_legend`) and are read once at startup. In
  the legend a word ending in `:` (`herd:`) starts a titled section. After
  changing one, `touch ~/forge/agent-sidebar/agent-sidebar` makes every live
  sidebar restart itself.
- `make` in that checkout replaces the binary every live sidebar runs. Build
  and test from a copy.
- The width is fixed at 28 and the legend is one row. Both were asked for.
- Amber is inferred: an agent idle longer than `@agent_sidebar_idle_wait`
  minutes shows as waiting. Claude never reports that state itself.

## Agents

If a workspace is configured (`~/.config/agent-sidebar/workspace`, see
`bin/agent-defs`), an agent there gets a slot: `<workspace>/Agent<N>/` holding
one worktree per repo in `AGENT_WS_REPOS`. One worktree cannot span several repos, and Claude keys its memory to the exact cwd, so the
agent is launched from the workspace root and told its slot in a brief.

- **`agent [dir]`** (`prefix a` picks the dir). For the workspace: take the
  first free slot or make the next one, put its worktrees on origin/main,
  write the brief, start the agent in a session named `<AGENT_WS_NAME>-a<N>` with
  a `board` window beside it. For any other dir: a plain agent in that repo,
  no slot. The session is stamped `@pm_slot` and `@pm_agent`.
  - Claude starts with `--name <session name>` so tmux, the sidebar and
    Claude's peer list agree on what a session is called.
  - A slot is free when no session holds it (by stamp, or by a pane inside
    it) and it has no uncommitted work under 48 h old. Older uncommitted work
    is reset on reuse. Committed branches always survive.
  - `--slots` lists every slot. `orphan` means a session still holds the slot
    with no agent left in it. `--dry-run` shows the pick.
- **`agent-pick`**: the workspace (if configured) pinned first, then zoxide dirs, `^f` finds
  any dir. Enter starts the agent used last; `^y` claude, `^o` opencode, `^x`
  codex.
- **`agent-defs`**: the registry the launcher, picker, roster and restore
  all source. An agent added there needs a launch command that takes a first
  prompt and a resume command.
- **`prefix q`, `agent-end`**: kill the session, clear its board status,
  reset its slot to origin/main. Uncommitted work in the slot is discarded.
- **`prefix r`, `agent --reset`**: start over. Same name and slot,
  worktrees back to origin/main, a new conversation.
- **`agent --restart [session]`**: new process, same conversation, nothing
  wiped. No key on purpose. Only claude resumes the exact conversation.
- **`prefix B`, `agent-branch`**: `claude --resume <id> --fork-session` in
  a new session. The fork takes no `@pm_slot`, so ending it never resets the
  parent's worktrees.
- **`prefix o`, `agent-reopen`**: pick a conversation that is on disk and
  not running (`agent-conversations` lists them, this project first, `^a` all)
  and resume it in its own session. It takes no `@pm_slot` either.
- **`agent --prune`**: prints a plan and changes nothing; `--yes` applies
  it. It ends orphaned sessions, drops stale ad-hoc worktrees in free slots,
  resets free slots holding stale edits and deletes local branches already in
  origin/main. Only on the owner's go.
- **`agent --rename`**: types `/rename <session name>` into live claude
  panes whose name has drifted. It skips busy panes and panes with a draft.
  Only on the owner's go.

`PM_WS` and `PM_REPOS` point all of this at a throwaway workspace for testing.
Sessions are targeted by id (`$N`), never by name: tmux reads a dot in a name
as window.pane.

## Board

`agent-board` is the fleet's quiet channel. `agent-board --help` is the manual; the
"Other agents" paragraph of `~/.claude/CLAUDE.md` is how agents learn it.
State is under `~/.local/state/agent-sidebar/`: `board.jsonl` (statuses),
`posts.jsonl` (notes), each with a `.log` of what ended.

- **`agent-board`** alone is an agent's briefing: its own status, unread notes,
  clashes that involve it, who else is on what.
- **Status**: `agent-board claim [path…] --what "…"`, one per tmux session,
  cleared by `agent-board done`. It lapses 2 h after the later of its last write
  and the agent's last activity. Paths are normalised so slots compare
  (`Agent6/helm/x` is `helm/x`), a path is resolved against the cwd, the
  slot, the workspace, `~/forge` and `~`, whichever has it, and a path
  outside `~/forge` is written from `~`. Overlapping another agent's path
  prints who holds it and exits 2; a claim on a whole slot repo (`app`,
  `helm`) clashes only with the same claim. When
  the sentence is over 90 minutes old the agent's next prompt carries one
  reminder.
- **Holds**: `agent-board hold NAME… --what "…"` before a release, a pin move
  or a prod sync; `agent-board free [NAME…]` after. While one agent has a name
  nobody else can take it: the command says who has it and exits 2, and the
  agent that was stopped gets a note when it is free. `all` stops every name.
  A hold lapses like a status, `agent-board done` frees it, and after 30 minutes
  the holder's next prompt reminds it. `agent-board holds [NAME…]` lists them
  and exits 2 when another agent has one, for scripts. `--pid N` ties a hold
  to a process: it goes when the process does, never lapses while it runs,
  and a second run by the same agent is stopped too. `release.sh` takes its
  service's hold this way, so an agent holds by hand only for what spans
  more than one run (pin moves, a prod sync). Holds lead the
  briefing and the board page. They replaced "please hold … until I post
  done" posts to everyone, which were 84% of those posts and were misread
  four times.
- **Notes**: `agent-board post "…" --to NAME`, `--path P` for whoever holds a
  path, or to everyone (3 an hour at most). NAME is a session name or a slot
  (`a7`); `agent-board reply ID "…"`;
  `agent-board read [--all]`. 600 characters at most. A thread lives 24 h past
  its last post, then moves to `posts.log`.
- **Delivery**: a note addressed to an agent, a reply in its thread or a post
  on a path it claims is added to its context after its next tool call
  (`agent-board-hook` on SessionStart and PostToolUse, `agent-board hook` on
  UserPromptSubmit). Posts to everyone are never pushed. An idle agent reads
  a note when it next works, and `post` says so; `SendMessage` wakes one.
- **OpenCode**: the sidebar's OpenCode plugin hands an agent its notes with
  the next prompt and after a tool call, and runs `agent-board guard PATH` before
  an edit: a path another agent holds is refused once, with the holder's
  name, and the repeat goes through. `agent-board post --wake` wakes an idle
  OpenCode addressee with the note. Where an agent's shell cannot reach tmux
  (a sandbox), the board takes who is calling and who is live from the
  sidebar's pane map, so keep the state directory on local disk: on NFS the
  map's lock stalls and a new session is missing from it for a while.
- **The `board` window** (`agent-board watch`, `prefix b`): messages on the left
  (`h` swaps in the last 7 days), and on the right holds, collisions, who
  needs you, who is on what, and what ended. A collision counts only
  between agents that worked in the last 24 hours; agents idle for over a day
  are one line under NEEDS YOU. It draws only while visible and
  restarts itself when `agent-board` changes and still compiles.

## Roster

`agent-roster` prints one line per live agent: the name a peer addresses, its
slot and the worktrees there that are on a branch, and its state. `-w` adds
the last thing each agent said, read from its transcript (`agent-lastsaid`), which
costs that agent nothing.

## Surviving a tmux restart

`tmux-resurrect` restores the layout from the last save, and a fresh server on
the default socket restores by itself. Saves are by hand, `prefix C-s`; the
30-minute timer is off.

`tmux/claude-panes.sh` runs from the resurrect hooks. On save it records every
agent pane with its conversation id and flags. On restore it relaunches each
one (`claude <flags> --name <session> --resume <id>`, `opencode --continue`,
`codex … resume --last`) and restarts the boards. `claude-panes.sh revive`
restarts any board left on a bare shell.

- The conversation id comes from the live process's
  `~/.claude/sessions/<pid>.json`. The pane tag `@claude_session` goes stale
  on resume and compact.
- `args` keeps flags, and the value of the few that take one. A positional
  prompt, `--name`, `--resume` and `--fork-session` are dropped.
- Only claude resumes the exact conversation. opencode continues the newest
  one for the dir, codex the newest one anywhere.
- A session that has never had a message has no transcript to resume.

## Claude settings

- `statusline.sh`: `<model> · <dir> · <branch> · <ctx%>`, with the model
  shown under an alias set at the top of the file.
- `themes/cyan.json`, used as `"theme": "custom:cyan"`. The bypass-permissions
  footer takes the `error` colour. `~/.claude/themes/` must exist when the CLI
  starts.
- `settings.json` hooks: `tmux/claude-pane-hook.sh` (tags the pane),
  `bin/agent-board-hook` and `agent-board hook`. Running sessions pick up hook
  changes without a restart.
- The startup banner cannot be changed by config. Patching the CLI is out.

## Open

- codex has no sidebar producer, so a codex agent shows no state.
- Messaging works only between Claude sessions. State and transcripts read
  for all three agents.
- `git/.gitconfig` in this public repo carries its owner's name. His call.
- The pre-scrub history lives only on the local tags
  `backup/pre-identity-scrub-20260910-{main,kubuntu}`.

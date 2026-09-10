# Producers

The sidebar reads agent state from files on disk. Each agent gets whatever
mechanism it actually offers, and the sidebar stays ignorant of the difference.

| agent | mechanism | who writes the file |
|---|---|---|
| Claude Code | native | Claude itself, `~/.claude/sessions/<pid>.json` |
| opencode | plugin | `producers/opencode/plugin.js`, via its lifecycle events |
| codex | plugin (todo) | in its hooks list upstream; unauthenticated here so far |

The opencode plugin is installed as a symlink at
`~/.config/opencode/plugins/agent-sidebar.js`. Verified live 2026-09-10: it
writes its file within ~8s of opencode starting, and removes it on exit, so a
killed agent leaves no stale row.

Reading what an agent last *said* is a separate thing and needs no producer at
all — every provider already streams its conversation to disk. `pm-lastsaid` in
the dotfiles repo has one adapter per format (claude JSONL, codex rollout JSONL,
opencode SQLite).

## The file format

One file per live agent process, named for its pid:

    $XDG_STATE_HOME/agent-sidebar/agents/<pid>.json

    {
      "pid": 12345,
      "procStart": "174097251",   // /proc/<pid>/stat field 22, guards pid reuse
      "status": "idle",           // waiting | idle | busy
      "cwd": "/home/you/project",
      "name": "my-project",
      "kind": "interactive"
    }

Deliberately the same shape Claude Code already writes, so the reader has one
code path. `procStart` is what makes a stale file harmless: if the pid has been
recycled the numbers disagree and the row is dropped.

A producer only has to write this file. It never talks to the sidebar, and the
sidebar never talks to it.

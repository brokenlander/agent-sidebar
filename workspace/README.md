# Herd in a workspace

Herd for a container workspace reached through a browser terminal, where the
workstation's slot layout and desktop do not exist. It brings the sidebar, the
board and the agent keys, and nothing of the workstation's tmux look.

    workspace/build-bundle.sh /tmp/herd-bundle.tgz                       # from this repo
    tar xzf herd-bundle.tgz -C ~ && ~/forge/agent-sidebar/workspace/install.sh   # in the workspace

The installer can be run again. `herd doctor` checks the result.

Keys after the prefix match the workstation: `a` then `c` or `o` starts Claude
Code or OpenCode in a folder picked from a list, `u` picks an agent, `q` ends
the agent in this session, `r` resets it, `o` reopens a conversation, `B`
branches one, `b` opens the board and goes back, `e` toggles the sidebar, `d`
opens a scratch terminal and `D` detaches. `q` and `r` work on any session
running an agent, including ones the workspace started itself.

Differences from the workstation:

- The sidebar's state lives in `/tmp/agent-sidebar-state`, linked from
  `~/.local/state/agent-sidebar`. On an NFS home the pane map's lock stalls
  and new sessions go missing from it.
- `herd new claude` marks the chosen folder trusted, so the agent starts at
  its prompt.
- An agent's session ends when the agent exits.
- The installer adds the board hooks to `~/.claude/settings.json` and the
  "Other agents" briefing to `~/.claude/CLAUDE.md` and
  `~/.config/opencode/AGENTS.md`.

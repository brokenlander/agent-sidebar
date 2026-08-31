/**
 * agent-sidebar producer for opencode.
 *
 * opencode stores no process identity anywhere in its database, so a row there
 * can never be tied back to a terminal pane. A plugin runs inside the opencode
 * process, which means process.pid is right here - so this writes the same
 * per-pid state file Claude Code writes natively, and the sidebar reads both
 * with one code path.
 *
 * Install:
 *   mkdir -p ~/.config/opencode/plugins
 *   ln -sf <repo>/producers/opencode/plugin.js ~/.config/opencode/plugins/agent-sidebar.js
 */

import { mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { basename, join } from "node:path"

const AGENT_DIR = join(
  process.env.XDG_STATE_HOME || join(process.env.HOME || "/tmp", ".local", "state"),
  "agent-sidebar",
  "agents",
)

/**
 * Field 22 of /proc/<pid>/stat. The comm field can contain spaces and
 * parentheses, so counting starts after the LAST ')': the token right after it
 * is field 3, putting starttime at index 19.
 *
 * The sidebar compares this against the live process before trusting the file,
 * so a crashed opencode leaves a row that is ignored rather than a ghost.
 */
function procStart(pid) {
  try {
    const stat = readFileSync(`/proc/${pid}/stat`, "utf8")
    const tail = stat.slice(stat.lastIndexOf(")") + 1).trim().split(/\s+/)
    return tail[19] ?? ""
  } catch {
    return ""
  }
}

/** Event type -> the state a person would act on. Anything else is ignored. */
function statusFor(type) {
  if (type.startsWith("permission.")) {
    return type === "permission.replied" ? "busy" : "waiting"
  }
  switch (type) {
    case "session.idle":
    case "session.error":
    case "session.interrupt":
      return "idle"
    case "session.created":
    case "message.updated":
    case "message.part.delta":
    case "message.part.updated":
      return "busy"
    default:
      return null
  }
}

export const AgentSidebar = async ({ directory, worktree }) => {
  const pid = process.pid
  const file = join(AGENT_DIR, `${pid}.json`)
  const cwd = directory || worktree || process.cwd()
  const start = procStart(pid)
  let current = null

  const write = (status) => {
    if (status === current) return // only on change: an idle agent writes nothing
    current = status
    try {
      mkdirSync(AGENT_DIR, { recursive: true })
      writeFileSync(
        file,
        JSON.stringify({
          pid,
          procStart: start,
          status,
          cwd,
          name: basename(cwd) || "opencode",
          kind: "interactive",
          agent: "opencode",
          statusUpdatedAt: Date.now(),
        }) + "\n",
      )
    } catch {
      /* the sidebar is a convenience; never break the agent over it */
    }
  }

  const remove = () => {
    try {
      rmSync(file, { force: true })
    } catch {
      /* ignore */
    }
  }

  write("idle")
  for (const sig of ["exit", "SIGINT", "SIGTERM", "SIGHUP"]) {
    process.on(sig, remove)
  }

  return {
    event: async ({ event }) => {
      const status = statusFor(event?.type ?? "")
      if (status) write(status)
    },
    dispose: async () => remove(),
  }
}

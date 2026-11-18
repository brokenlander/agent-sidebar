/**
 * agent-sidebar producer for opencode.
 *
 * opencode stores no process identity anywhere in its database, so a row there
 * can never be tied back to a terminal pane. A plugin runs inside the opencode
 * process, which means process.pid is right here - so this writes the same
 * per-pid state file the sidebar already reads natively, and it reads both
 * with one code path.
 *
 * Install:
 *   mkdir -p ~/.config/opencode/plugins
 *   ln -sf <repo>/producers/opencode/plugin.js ~/.config/opencode/plugins/agent-sidebar.js
 */

import { spawnSync } from "node:child_process"
import { mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs"
import { basename, join } from "node:path"

const STATE_DIR = join(
  process.env.XDG_STATE_HOME || join(process.env.HOME || "/tmp", ".local", "state"),
  "agent-sidebar",
)
const AGENT_DIR = join(STATE_DIR, "agents")
const BOARD = process.env.PM_BOARD || join(process.env.HOME || "/tmp", "dotfiles", "bin", "pm-board")
const PANE = process.env.TMUX_PANE || ""
const EDIT_TOOLS = new Set(["edit", "write", "patch", "multiedit"])
const WAKE_EVERY_MS = 3000

function mtime(path) {
  try {
    return statSync(path).mtimeMs
  } catch {
    return 0
  }
}

/** This pane's tmux session id without its $, from the map the sidebar keeps. */
function sessionId() {
  const sock = basename((process.env.TMUX || "").split(",")[0]) || "default"
  try {
    for (const line of readFileSync(join(STATE_DIR, `panes-${sock}`), "utf8").split("\n")) {
      const f = line.split("\t")
      if (f[2] === PANE) return (f[1] ?? "").replace("$", "")
    }
  } catch {
    /* no map yet */
  }
  return ""
}

function board(args, input) {
  try {
    return spawnSync(BOARD, args, { input: input ?? "", encoding: "utf8", timeout: 5000 })
  } catch {
    return { status: 0, stdout: "", stderr: "" }
  }
}

/** Notes addressed to this agent that it has not seen, as the board hook words them. */
function unseenNotes(event) {
  if (!PANE) return ""
  const posts = mtime(join(STATE_DIR, "posts.jsonl"))
  if (!posts || posts <= mtime(join(STATE_DIR, "seen", PANE.replace("%", "")))) return ""
  const r = board(["hook"], JSON.stringify({ hook_event_name: event }))
  try {
    return JSON.parse(r.stdout || "{}").hookSpecificOutput?.additionalContext ?? ""
  } catch {
    return ""
  }
}

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

export const AgentSidebar = async ({ directory, worktree, client }) => {
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

  let sessionID = ""
  let reportsStatus = false
  const warned = new Set()

  const wake = setInterval(() => {
    const me = sessionId()
    const flag = me && join(STATE_DIR, "wake", me)
    if (!flag || !mtime(flag)) return
    if (current !== "idle" || !sessionID) return
    rmSync(flag, { force: true })
    const notes = unseenNotes("UserPromptSubmit")
    if (!notes) return
    client?.session
      ?.promptAsync({ path: { id: sessionID }, body: { parts: [{ type: "text", text: notes }] } })
      ?.catch?.(() => {})
  }, WAKE_EVERY_MS)
  wake.unref?.()

  return {
    event: async ({ event }) => {
      const type = event?.type ?? ""
      if (type === "session.status") {
        reportsStatus = true
        write(event?.properties?.status?.type === "idle" ? "idle" : "busy")
        return
      }
      if (reportsStatus && type.startsWith("message.")) return
      const status = statusFor(type)
      if (status) write(status)
    },
    "chat.message": async (input, output) => {
      sessionID = input?.sessionID || sessionID
      const notes = unseenNotes("UserPromptSubmit")
      const text = notes && output?.parts?.findLast?.((p) => p.type === "text")
      if (text) text.text += `\n\n${notes}`
    },
    "tool.execute.before": async (input, output) => {
      sessionID = input?.sessionID || sessionID
      const path = output?.args?.filePath || output?.args?.path
      if (!PANE || !EDIT_TOOLS.has(input?.tool) || !path) return
      const r = board(["guard", path])
      const said = (r.stdout || "").trim()
      if (r.status !== 2 || !said || warned.has(path)) return
      warned.add(path)
      throw new Error(said)
    },
    "tool.execute.after": async (input, output) => {
      sessionID = input?.sessionID || sessionID
      const notes = unseenNotes("PostToolUse")
      if (notes && output && typeof output.output === "string") output.output += `\n\n${notes}`
    },
    dispose: async () => {
      clearInterval(wake)
      remove()
    },
  }
}

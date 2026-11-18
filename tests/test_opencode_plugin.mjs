/* Exercises the opencode producer without needing a model: construct the
   plugin, fire the events opencode emits, and assert the state file. */
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

const state = mkdtempSync(join(tmpdir(), "agent-sidebar-test-"))
process.env.XDG_STATE_HOME = state

/* A stand-in board: `hook` hands back one note, `guard held.txt` refuses. */
const dir = join(state, "agent-sidebar")
const fakeBoard = join(state, "pm-board")
mkdirSync(join(dir, "seen"), { recursive: true })
writeFileSync(fakeBoard, `#!/bin/sh
case "$1" in
  hook) touch "${dir}/seen/7"
        echo '{"hookSpecificOutput":{"additionalContext":"[abc12] other, to you: NOTE-TEXT"}}' ;;
  guard) case "$2" in *held.txt) echo "held.txt is held on the board by other"; exit 2 ;; esac ;;
esac
`)
chmodSync(fakeBoard, 0o755)
process.env.PM_BOARD = fakeBoard
process.env.TMUX_PANE = "%7"
process.env.TMUX = "/tmp/tmux-0/sock,1,0"
writeFileSync(join(dir, "panes-sock"), "/dev/pts/9\t$3\t%7\tme\n")
const post = () => {
  writeFileSync(join(dir, "posts.jsonl"), "{}\n")
  rmIfThere(join(dir, "seen", "7"))
}
const rmIfThere = (p) => { try { rmSync(p) } catch { /* absent */ } }

const { AgentSidebar } = await import("../producers/opencode/plugin.js")
const file = join(state, "agent-sidebar", "agents", `${process.pid}.json`)

let fails = 0
const check = (cond, what) => {
  if (!cond) { console.log(`  FAIL  ${what}`); fails++ }
}
const read = () => JSON.parse(readFileSync(file, "utf8"))

const prompts = []
const client = { session: { promptAsync: async (req) => { prompts.push(req) } } }
const hooks = await AgentSidebar({ directory: "/home/someone/myproject", worktree: "", client })

check(existsSync(file), "writes a state file on init")
check(read().status === "idle", "starts idle")
check(read().pid === process.pid, "records its own pid")
check(read().name === "myproject", "derives a name from the directory")
check(read().agent === "opencode", "labels the producer")

/* procStart must match what the sidebar reads, or every row is dropped */
const stat = readFileSync(`/proc/${process.pid}/stat`, "utf8")
const truth = stat.slice(stat.lastIndexOf(")") + 1).trim().split(/\s+/)[19]
check(read().procStart === truth, "procStart matches /proc field 22")

const fire = async (type) => hooks.event({ event: { type } })

await fire("message.part.delta");  check(read().status === "busy", "streaming -> busy")
await fire("session.idle");        check(read().status === "idle", "session.idle -> idle")
await fire("permission.ask");      check(read().status === "waiting", "permission.ask -> waiting")
await fire("permission.replied");  check(read().status === "busy", "permission.replied -> busy")
await fire("session.error");       check(read().status === "idle", "session.error -> idle")

const before = read().statusUpdatedAt
await new Promise(r => setTimeout(r, 5))
await fire("session.idle")
check(read().statusUpdatedAt === before, "no rewrite when the status is unchanged")

await fire("some.unknown.event")
check(read().status === "idle", "unknown events are ignored")

/* opencode 1.19 sends message.updated after session.idle */
const status = async (type) => hooks.event({ event: { type: "session.status", properties: { status: { type } } } })
await status("busy");              check(read().status === "busy", "session.status busy -> busy")
await status("idle");              check(read().status === "idle", "session.status idle -> idle")
await fire("message.updated");     check(read().status === "idle", "a late message.updated does not undo idle")
await fire("permission.ask");      check(read().status === "waiting", "permission.ask still -> waiting")
await status("idle")

post()
const tool = { title: "", output: "RESULT", metadata: {} }
await hooks["tool.execute.after"]({ tool: "bash", sessionID: "ses_1", callID: "c", args: {} }, tool)
check(tool.output.startsWith("RESULT") && tool.output.includes("NOTE-TEXT"), "a note rides on the tool result")
const again = { title: "", output: "RESULT", metadata: {} }
await hooks["tool.execute.after"]({ tool: "bash", sessionID: "ses_1", callID: "c", args: {} }, again)
check(again.output === "RESULT", "a seen note is not delivered twice")

post()
const said = { message: {}, parts: [{ type: "text", text: "hello" }] }
await hooks["chat.message"]({ sessionID: "ses_1" }, said)
check(said.parts[0].text.startsWith("hello") && said.parts[0].text.includes("NOTE-TEXT"), "a note rides on the prompt")

const edit = (filePath) => hooks["tool.execute.before"]({ tool: "edit", sessionID: "ses_1", callID: "c" }, { args: { filePath } })
let refused = ""
await edit("/w/held.txt").catch((e) => { refused = e.message })
check(refused.includes("held on the board"), "an edit of a held file is refused with the holder")
refused = ""
await edit("/w/held.txt").catch((e) => { refused = e.message })
check(refused === "", "the same edit goes through the second time")
await edit("/w/free.txt").catch((e) => { refused = e.message })
check(refused === "", "a free file is never refused")
await hooks["tool.execute.before"]({ tool: "read", sessionID: "ses_1", callID: "c" }, { args: { filePath: "/w/held.txt" } })
  .catch((e) => { refused = e.message })
check(refused === "", "reading a held file is not refused")

post()
mkdirSync(join(dir, "wake"), { recursive: true })
writeFileSync(join(dir, "wake", "3"), "abc12")
await new Promise(r => setTimeout(r, 3500))
check(prompts.length === 1 && prompts[0].path.id === "ses_1", "a wake flag prompts the idle session once")
check(prompts[0]?.body.parts[0].text.includes("NOTE-TEXT"), "the wake prompt carries the note")
check(!existsSync(join(dir, "wake", "3")), "the wake flag is consumed")

await hooks.dispose()
check(!existsSync(file), "dispose removes the file")

console.log(fails === 0 ? "  OK    all checks passed" : `  ${fails} check(s) failed`)
process.exit(fails === 0 ? 0 : 1)

/* Exercises the opencode producer without needing a model: construct the
   plugin, fire the events opencode emits, and assert the state file. */
import { mkdtempSync, readFileSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

const state = mkdtempSync(join(tmpdir(), "agent-sidebar-test-"))
process.env.XDG_STATE_HOME = state

const { AgentSidebar } = await import("../producers/opencode/plugin.js")
const file = join(state, "agent-sidebar", "agents", `${process.pid}.json`)

let fails = 0
const check = (cond, what) => {
  if (!cond) { console.log(`  FAIL  ${what}`); fails++ }
}
const read = () => JSON.parse(readFileSync(file, "utf8"))

const hooks = await AgentSidebar({ directory: "/home/someone/myproject", worktree: "" })

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

await hooks.dispose()
check(!existsSync(file), "dispose removes the file")

console.log(fails === 0 ? "  OK    all checks passed" : `  ${fails} check(s) failed`)
process.exit(fails === 0 ? 0 : 1)

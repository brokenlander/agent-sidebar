#!/usr/bin/env python3
"""agent-board with tmux out of reach, as in a sandboxed agent's shell: identity
and liveness must come from the sidebar's pane map. Usage: agent-board-notmux.py [PM-BOARD]"""
import json, os, pty, shutil, subprocess, sys, tempfile, time
from pathlib import Path

board = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).resolve().parents[1] / "bin" / "agent-board")
tmp = Path(tempfile.mkdtemp(prefix="agent-board-notmux-"))
state = tmp / "state" / "agent-sidebar"
(state / "agents").mkdir(parents=True)
(tmp / "bin").mkdir()
(tmp / "bin" / "tmux").write_text("#!/bin/sh\nexit 1\n")
(tmp / "bin" / "tmux").chmod(0o755)
shutil.copy("/bin/sleep", tmp / "bin" / "opencode")

agents = {}
for name, pane in (("me", "%7"), ("other", "%8")):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(tmp / "bin" / "opencode"), ["opencode", "60"])
    agents[name] = (pid, os.readlink(f"/proc/{pid}/fd/0"), pane, fd)
time.sleep(0.3)
(state / "panes-default").write_text("".join(
    f"{tty}\t${i}\t{pane}\t{name}\n" for i, (name, (pid, tty, pane, _)) in enumerate(agents.items())))
for name, (pid, tty, pane, _) in agents.items():
    (state / "agents" / f"{pid}.json").write_text(json.dumps(
        {"pid": pid, "status": "idle", "cwd": str(tmp), "name": name, "statusUpdatedAt": int(time.time() * 1000)}))

fails = 0
def run(who, *args):
    env = dict(os.environ, PATH=f"{tmp / 'bin'}:{os.environ['PATH']}", XDG_STATE_HOME=str(tmp / "state"),
               HOME=str(tmp), TMUX="/tmp/tmux-0/default,1,0", TMUX_PANE=agents[who][2])
    r = subprocess.run([sys.executable, board, *args], capture_output=True, text=True, env=env, cwd=tmp, stdin=subprocess.DEVNULL, timeout=20)
    return r.returncode, r.stdout + r.stderr
def check(cond, what, shown=""):
    global fails
    if not cond:
        fails += 1
        print(f"  FAIL  {what}\n{shown}")

code, out = run("other", "claim", "f.txt", "--what", "holding f")
check(code == 0 and "on the board as other" in out, "a claim names the caller from the pane map", out)
code, out = run("me", "claim", "f.txt", "--what", "also f")
check(code == 2 and "held by other" in out, "a clash names the live holder", out)
code, out = run("me")
check("other" in out and "gone" not in out, "the listing shows the other agent as live", out)
code, out = run("me", "guard", "f.txt")
check(code == 2 and "held on the board by other" in out, "guard refuses a held path", out)
code, out = run("me", "guard", "free.txt")
check(code == 0 and not out.strip(), "guard is silent on a free path", out)
code, out = run("me", "post", "--wake", "ping", "--to", "other")
check(code == 0 and "waking other" in out and (state / "wake" / "1").exists(), "post --wake flags an idle opencode agent", out)
code, out = run("other", "hook")
check("ping" in out, "the hook hands the addressee its note", out)
code, out = run("me", "post", "x", "--to", "nobody")
check(code != 0 and "Live: me, other" in out, "an unknown addressee lists who is live", out)

for pid, _, _, fd in agents.values():
    os.kill(pid, 9)
shutil.rmtree(tmp, ignore_errors=True)
print("  OK    all checks passed" if not fails else f"  {fails} check(s) failed")
sys.exit(1 if fails else 0)

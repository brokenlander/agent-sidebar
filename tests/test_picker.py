#!/usr/bin/env python3
"""Drives the picker through a real popup on a private tmux server.

The popup only exists on an attached client, so one is attached on a pty and
the keys go in through that. This is where alt-n deadlocked: the prompt it
opens blocked the popup's shell, and the popup took the keys the prompt needed.
"""
import fcntl, json, os, pty, re, select, shutil, struct, subprocess, sys, tempfile, termios, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "agent-sidebar")
sock = "agent-sidebar-picker-%d" % os.getpid()
tmp = tempfile.mkdtemp(prefix="agent-sidebar-picker")
env = dict(os.environ)
env.pop("TMUX", None)
env.pop("TMUX_PANE", None)
env.update(CLAUDE_CONFIG_DIR=tmp + "/claude", XDG_STATE_HOME=tmp + "/state",
           TERM="xterm-256color")
for d in ("/claude/sessions", "/work", "/state"):
    os.makedirs(tmp + d)

fails = 0
def check(ok, what):
    global fails
    print("  %-5s %s" % ("ok" if ok else "FAIL", what))
    if not ok:
        fails += 1

def tm(*args):
    return subprocess.run(["tmux", "-L", sock, *args], env=env,
                          capture_output=True, text=True).stdout

def wait_for(cond, secs):
    end = time.time() + secs
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.2)
    return False

print("picker tests")
try:
    tm("new-session", "-d", "-s", "host", "-x", "120", "-y", "40", "sleep 600")
    tm("set", "-g", "@agent_sidebar_command_claude", "sleep 30")   # never a real agent

    # an agent to pick: the sleep in host, with a state file for its pid
    pid = tm("list-panes", "-t", "host", "-F", "#{pane_pid}").split()[0]
    stat = open("/proc/%s/stat" % pid).read()
    start = stat[stat.rindex(")") + 2:].split()[19]
    with open("%s/claude/sessions/%s.json" % (tmp, pid), "w") as f:
        json.dump({"pid": int(pid), "procStart": start, "status": "idle",
                   "cwd": tmp + "/work", "name": "host", "kind": "interactive"}, f)

    child, fd = pty.fork()
    if child == 0:
        os.execvpe("tmux", ["tmux", "-L", sock, "attach", "-t", "host"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

    raw = b""
    def drain():
        global raw
        while True:
            r, _, _ = select.select([fd], [], [], 0.05)
            if not r:
                return
            try:
                raw += os.read(fd, 65536)
            except OSError:
                return
    def seen(text):
        drain()
        return text.encode() in re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", raw)

    check(wait_for(lambda: tm("list-clients", "-F", "#{client_name}").strip() != "", 5),
          "a client attaches on a pty")
    client = tm("list-clients", "-F", "#{client_name}").split()[0]

    popup = subprocess.Popen(["tmux", "-L", sock, "display-popup", "-c", client,
                              "-w", "95%", "-h", "95%", "-E",
                              os.path.join(ROOT, "scripts", "picker.sh")], env=env)
    check(wait_for(lambda: seen("jump"), 5), "the picker opens in a popup")

    os.write(fd, b"\x1bn")                                     # alt-n
    check(wait_for(lambda: popup.poll() is not None, 5), "alt-n closes the popup")
    check(wait_for(lambda: seen("agent in:"), 3), "and asks where to start the agent")

    os.write(fd, b"\r")                                        # accept the row's directory
    check(wait_for(lambda: "work" in tm("list-sessions", "-F", "#{session_name}").split(), 5),
          "enter starts a session named for the directory")
    check(tm("display-message", "-p", "-t", "work", "#{pane_current_path}").strip() == tmp + "/work",
          "in that directory")
finally:
    tm("kill-server")
    shutil.rmtree(tmp, ignore_errors=True)

print()
if fails:
    print("  %d picker check(s) failed" % fails)
    sys.exit(1)
print("  OK    all picker checks passed")

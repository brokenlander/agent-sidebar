#!/usr/bin/env python3
"""The middle-click menu must survive letting go of the button.

A menu opened by a process (not a tmux mouse binding) is marked "no mouse" by
tmux, so the release of the very button that opened it used to close it again -
the menu flashed and vanished. The fix passes -M -O to display-menu. -M is a
tmux 3.5 feature, so on older servers this test skips.

The menu is a client overlay, invisible to capture-pane, so it is tested by
effect: open it, let go, press a menu key, and check the action ran.
"""
import fcntl, json, os, pty, re, select, shutil, struct, subprocess, sys, tempfile, termios, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "agent-sidebar")
sock = "agent-sidebar-menu-%d" % os.getpid()
tmp = tempfile.mkdtemp(prefix="agent-sidebar-menu")
env = dict(os.environ)
env.pop("TMUX", None); env.pop("TMUX_PANE", None)
env.update(CLAUDE_CONFIG_DIR=tmp + "/claude", XDG_STATE_HOME=tmp + "/state",
           TERM="xterm-256color")
for d in ("/claude/sessions", "/work", "/state"):
    os.makedirs(tmp + d)

def tm(*a):
    return subprocess.run(["tmux", "-L", sock, *a], env=env,
                          capture_output=True, text=True).stdout

ver = subprocess.run(["tmux", "-V"], capture_output=True, text=True).stdout.split()[-1]
m = re.match(r"(\d+)\.(\d+)", ver)
if not m or (int(m.group(1)), int(m.group(2))) < (3, 5):
    print("menu tests")
    print("  SKIP  tmux %s has no -M; the mouse-menu fix needs 3.5+" % ver)
    sys.exit(0)

fails = 0
def check(ok, what):
    global fails
    print("  %-5s %s" % ("ok" if ok else "FAIL", what))
    if not ok:
        fails += 1

print("menu tests")
child = None
try:
    tm("new-session", "-d", "-s", "alpha", "-x", "120", "-y", "40", "sleep 600")
    tm("set", "-g", "mouse", "on")
    pid = tm("list-panes", "-t", "alpha", "-F", "#{pane_pid}").split()[0]
    stat = open("/proc/%s/stat" % pid).read()
    start = stat[stat.rindex(")") + 2:].split()[19]
    with open("%s/claude/sessions/%s.json" % (tmp, pid), "w") as f:
        json.dump({"pid": int(pid), "procStart": start, "status": "idle",
                   "cwd": tmp + "/work", "name": "alpha", "kind": "interactive"}, f)
    tm("new-session", "-d", "-s", "bar", "-x", "120", "-y", "40")
    tm("split-window", "-h", "-b", "-l", "30", "-d", "-t", "bar", BIN)

    child, fd = pty.fork()
    if child == 0:
        os.execvpe("tmux", ["tmux", "-L", sock, "attach", "-t", "bar"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

    def drain(t=0.3):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if not r:
                continue
            try:
                os.read(fd, 65536)
            except OSError:
                return
    def wait(cond, secs):
        end = time.time() + secs
        while time.time() < end:
            drain(0.2)
            if cond():
                return True
        return False
    def screen():
        return re.sub(r"\x1b\[[0-9;]*m", "", tm("capture-pane", "-p", "-t", pane))
    def parked():
        return os.path.exists("%s/state/agent-sidebar/parked" % tmp)

    check(wait(lambda: tm("list-clients", "-F", "#{client_name}").strip() != "", 5),
          "a client attaches on a pty")
    pane = [l.split()[0] for l in
            tm("list-panes", "-t", "bar", "-F", "#{pane_id} #{pane_current_command}").splitlines()
            if l.endswith("agent-sidebar")][0]
    check(wait(lambda: "alpha" in screen(), 5), "the sidebar lists the agent")

    # find the terminal row of the agent by which left-click actually jumps to it
    term_row = None
    for cand in range(3, 9):
        tm("switch-client", "-t", "bar"); time.sleep(0.2); drain()
        os.write(fd, ("\x1b[<0;3;%dM" % cand).encode()); time.sleep(0.15)
        os.write(fd, ("\x1b[<0;3;%dm" % cand).encode())
        if wait(lambda: tm("list-clients", "-F", "#{client_session}").strip() == "alpha", 1.5):
            term_row = cand
            break
    check(term_row is not None, "a row is clickable")
    tm("switch-client", "-t", "bar"); time.sleep(0.3); drain()

    # press and HOLD the middle button, then let go: the menu must still be up
    os.write(fd, ("\x1b[<1;3;%dM" % term_row).encode())
    time.sleep(0.5)
    os.write(fd, ("\x1b[<1;3;%dm" % term_row).encode())  # release
    time.sleep(0.6); drain()
    os.write(fd, b"p")                                    # choose Park by key
    check(wait(parked, 3), "the menu survives the release and a choice works")
finally:
    tm("kill-server")
    shutil.rmtree(tmp, ignore_errors=True)

print()
if fails:
    print("  %d menu check(s) failed" % fails)
    sys.exit(1)
print("  OK    all menu checks passed")

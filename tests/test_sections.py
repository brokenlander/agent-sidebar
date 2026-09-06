#!/usr/bin/env python3
"""The sidebar's optional sections: non-agent tmux sessions and a key legend.

With @agent_sidebar_sessions on, sessions holding no agent are listed below the
agents and are clickable to jump. @agent_sidebar_legend paints a key cheatsheet
at the bottom.
"""
import fcntl, json, os, pty, re, select, shutil, struct, subprocess, sys, tempfile, termios, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "agent-sidebar")
sock = "agent-sidebar-sections-%d" % os.getpid()
tmp = tempfile.mkdtemp(prefix="agent-sidebar-sections")
env = dict(os.environ)
env.pop("TMUX", None); env.pop("TMUX_PANE", None)
env.update(CLAUDE_CONFIG_DIR=tmp + "/claude", XDG_STATE_HOME=tmp + "/state",
           TERM="xterm-256color")
for d in ("/claude/sessions", "/state", "/work"):
    os.makedirs(tmp + d)

fails = 0
def check(ok, what):
    global fails
    print("  %-5s %s" % ("ok" if ok else "FAIL", what))
    if not ok:
        fails += 1

def tm(*a):
    return subprocess.run(["tmux", "-L", sock, *a], env=env,
                          capture_output=True, text=True).stdout

print("sections tests")
try:
    tm("new-session", "-d", "-s", "alpha", "-x", "100", "-y", "44", "sleep 600")
    tm("new-session", "-d", "-s", "plainbox", "-x", "100", "-y", "44", "sleep 600")
    tm("new-session", "-d", "-s", "victim", "-x", "100", "-y", "44", "sleep 600")
    tm("set", "-g", "@agent_sidebar_sessions", "on")
    tm("set", "-g", "@agent_sidebar_legend", "e sidebar|o agents|y sesh")
    tm("set", "-g", "mouse", "on")

    pid = tm("list-panes", "-t", "alpha", "-F", "#{pane_pid}").split()[0]
    stat = open("/proc/%s/stat" % pid).read()
    start = stat[stat.rindex(")") + 2:].split()[19]
    with open("%s/claude/sessions/%s.json" % (tmp, pid), "w") as f:
        json.dump({"pid": int(pid), "procStart": start, "status": "idle",
                   "cwd": tmp + "/work", "name": "alpha", "kind": "interactive"}, f)

    tm("new-session", "-d", "-s", "host", "-x", "120", "-y", "44")
    tm("split-window", "-h", "-b", "-l", "34", "-d", "-t", "host", BIN)

    child, fd = pty.fork()
    if child == 0:
        os.execvpe("tmux", ["tmux", "-L", sock, "attach", "-t", "host"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 44, 120, 0, 0))

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
    def view():
        return re.sub(r"\x1b\[[0-9;]*m", "", tm("capture-pane", "-p", "-t", pane))

    check(wait(lambda: tm("list-clients", "-F", "#{client_name}").strip() != "", 5),
          "a client attaches on a pty")
    pane = [l.split()[0] for l in
            tm("list-panes", "-t", "host", "-F", "#{pane_id} #{pane_current_command}").splitlines()
            if l.endswith("agent-sidebar")][0]
    check(wait(lambda: "alpha" in view(), 5), "the agent is listed")
    check(wait(lambda: "sessions" in view() and "plainbox" in view(), 5),
          "a non-agent session is listed in its own section")
    check("alpha" not in view().split("sessions")[1] if "sessions" in view() else False,
          "the agent session is not duplicated under sessions")
    check("keys" in view() and "sesh" in view(), "the key legend is shown")

    # click the plainbox row -> the client jumps to it
    rows = view().split("\n")
    prow = [i + 1 for i, l in enumerate(rows) if "plainbox" in l]
    jumped = False
    if prow:
        for cand in range(prow[0] - 1, prow[0] + 2):
            tm("switch-client", "-t", "host"); time.sleep(0.2); drain(0.3)
            os.write(fd, ("\x1b[<0;5;%dM" % cand).encode()); time.sleep(0.1)
            os.write(fd, ("\x1b[<0;5;%dm" % cand).encode())
            if wait(lambda: tm("list-clients", "-F", "#{client_session}").strip() == "plainbox", 2):
                jumped = True
                break
    check(jumped, "clicking a session row jumps to it")

    # middle-click a session -> menu -> Kill -> confirm removes it
    ver = subprocess.run(["tmux", "-V"], capture_output=True, text=True).stdout.split()[-1]
    m = re.match(r"(\d+)\.(\d+)", ver)
    if m and (int(m.group(1)), int(m.group(2))) >= (3, 5):
        tm("switch-client", "-t", "host"); time.sleep(0.3); drain(0.3)
        rows = view().split("\n")
        vrow = [i + 1 for i, l in enumerate(rows) if "victim" in l]
        # calibrate the victim row: which terminal row a left-click jumps from
        term = None
        if vrow:
            for cand in range(vrow[0] - 1, vrow[0] + 2):
                tm("switch-client", "-t", "host"); time.sleep(0.2); drain(0.3)
                os.write(fd, ("\x1b[<0;5;%dM" % cand).encode()); time.sleep(0.1)
                os.write(fd, ("\x1b[<0;5;%dm" % cand).encode())
                if wait(lambda: tm("list-clients", "-F", "#{client_session}").strip() == "victim", 2):
                    term = cand
                    break
        killed = False
        if term is not None:
            tm("switch-client", "-t", "host"); time.sleep(0.3); drain(0.3)
            os.write(fd, ("\x1b[<1;5;%dM" % term).encode()); time.sleep(0.4)  # middle press
            os.write(fd, ("\x1b[<1;5;%dm" % term).encode()); time.sleep(0.5); drain(0.3)  # release
            os.write(fd, b"k"); time.sleep(0.4); drain(0.3)   # Kill
            os.write(fd, b"y"); time.sleep(0.4); drain(0.3)   # confirm
            killed = wait(lambda: "victim" not in tm("list-sessions", "-F", "#{session_name}").split(), 3)
        check(killed, "middle-click a session -> Kill removes it")
    else:
        print("  SKIP  middle-click kill (tmux %s has no -M)" % ver)
finally:
    tm("kill-server")
    shutil.rmtree(tmp, ignore_errors=True)

print()
if fails:
    print("  %d sections check(s) failed" % fails)
    sys.exit(1)
print("  OK    all sections checks passed")

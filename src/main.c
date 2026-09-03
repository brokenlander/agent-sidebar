#define _POSIX_C_SOURCE 200809L

#include "agents.h"
#include "render.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_winch;
static volatile sig_atomic_t g_stop;

static void on_winch(int sig)
{
	(void)sig;
	g_winch = 1;
}

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
}

static long long now_ms(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0)
		return 0;
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void term_size(int fd, int *cols, int *rows)
{
	struct winsize ws;

	if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return;
	}
	*cols = 32;
	*rows = 40;
}

static void producer_dir(char *dst, size_t cap)
{
	const char *xdg = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");

	if (xdg != NULL && *xdg != '\0')
		snprintf(dst, cap, "%s/agent-sidebar/agents", xdg);
	else
		snprintf(dst, cap, "%s/.local/state/agent-sidebar/agents",
			 home != NULL ? home : ".");
	mkdir(dst, 0755); /* so the watch can be established before a producer runs */
}

static void sessions_dir(char *dst, size_t cap)
{
	const char *cfg = getenv("CLAUDE_CONFIG_DIR");

	if (cfg != NULL && *cfg != '\0') {
		snprintf(dst, cap, "%s/sessions", cfg);
		return;
	}
	const char *home = getenv("HOME");
	snprintf(dst, cap, "%s/.claude/sessions", home != NULL ? home : ".");
}

static void trace(const char *fmt, ...);

static struct termios g_saved_tio;
static int g_tio_saved;

static void emit(const char *s)
{
	ssize_t r = write(STDOUT_FILENO, s, strlen(s));
	(void)r;
}

/* Registered with atexit and called from the signal path, so the pane is never
   left in raw mode with mouse reporting on. */
static void tty_restore(void)
{
	emit("\033[?1006l\033[?1000l\033[?25h");
	if (g_tio_saved)
		tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
}

static void tty_setup(void)
{
	struct termios t;

	if (tcgetattr(STDIN_FILENO, &t) == 0) {
		g_saved_tio = t;
		g_tio_saved = 1;
		/* no line buffering, no echo of the mouse escape sequences */
		t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &t);
	}
	/* 1000 = button press/release, 1006 = SGR encoding (no 223 column cap) */
	emit("\033[?1000h\033[?1006h");
}

/* Run tmux with an argv array. No shell is involved, so a session name may
   contain spaces, quotes or anything else without quoting or an allowlist. */
static int run_tmux(char *const argv[])
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		/* Errors reach the user through notify() and exit codes. Left
		   inherited, tmux's stderr would be printed into the sidebar's
		   own pane and corrupt the display - a has-session probe alone
		   writes "can't find session" every time. */
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
			close(null);
		}
		execvp("tmux", argv);
		_exit(127);
	}

	int st = 0;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Capture all of stdout, newlines intact. */
static int capture_all(char *const argv[], char *out, size_t cap);

/* Capture all of stdout, newlines intact. */
static int capture_all(char *const argv[], char *out, size_t cap)
{
	int fds[2];

	if (cap == 0 || pipe(fds) != 0)
		return -1;
	out[0] = '\0';

	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		execvp("tmux", argv);
		_exit(127);
	}

	close(fds[1]);
	size_t got = 0;
	while (got < cap - 1) {
		ssize_t r = read(fds[0], out + got, cap - 1 - got);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(fds[0]);
	out[got] = '\0';

	int st = 0;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}



/* Failures used to be swallowed by 2>/dev/null, which made a rejected rename
   look like a crash. Say so on the status line instead. */
static void notify(const char *msg)
{
	char *argv[] = { (char *)"tmux", (char *)"display-message",
			 (char *)"--", (char *)msg, NULL };
	run_tmux(argv);
}


/* Move the client that is looking at this sidebar to the clicked agent. The
   client is resolved from our own session rather than left to tmux's notion of
   "current", which is ambiguous with several clients attached. */
static void jump_to(const agent *a)
{
	char client[128] = "";

	if (a->pane_id[0] == '\0')
		return;

	/* Key on our own pane id, never the session name: a rename left the
	   cached name pointing at nothing, the client came back empty, and the
	   switch fell through to whichever client tmux considered current -
	   which with several attached is usually a different terminal. Pane ids
	   do not change, and the lookup is one 4ms call. */
	const char *self_pane = getenv("TMUX_PANE");
	if (self_pane != NULL && *self_pane != '\0') {
		char list[2048];
		char *lc[] = { (char *)"tmux", (char *)"list-clients",
			       (char *)"-t", (char *)self_pane, (char *)"-F",
			       (char *)"#{client_activity} #{client_name}",
			       NULL };
		if (capture_all(lc, list, sizeof list) == 0) {
			/* Two terminals can view one session. Prefer the one
			   used most recently - it is the one in front of
			   whoever clicked. */
			long long best = -1;
			char *save = NULL;
			for (char *line = strtok_r(list, "\n", &save);
			     line != NULL;
			     line = strtok_r(NULL, "\n", &save)) {
				char *sp = strchr(line, ' ');
				if (sp == NULL)
					continue;
				*sp = '\0';
				long long act = atoll(line);
				if (act > best) {
					best = act;
					snprintf(client, sizeof client, "%s",
						 sp + 1);
				}
			}
		}
	}

	char *with_client[] = {
		(char *)"tmux",
		(char *)"switch-client", (char *)"-c", client,
		(char *)"-t", (char *)a->sess, (char *)";",
		(char *)"select-window", (char *)"-t", (char *)a->pane_id,
		(char *)";",
		(char *)"select-pane", (char *)"-t", (char *)a->pane_id,
		NULL,
	};
	char *no_client[] = {
		(char *)"tmux",
		(char *)"switch-client", (char *)"-t", (char *)a->sess,
		(char *)";",
		(char *)"select-window", (char *)"-t", (char *)a->pane_id,
		(char *)";",
		(char *)"select-pane", (char *)"-t", (char *)a->pane_id,
		NULL,
	};

	int rc = run_tmux(client[0] != '\0' ? with_client : no_client);

	trace("jump sess=%s pane=%s client=%s rc=%d", a->sess, a->pane_id,
	      client, rc);
}

/* tmux renders the menu, handles its keys and runs the chosen command, so the
   sidebar needs no menu widget of its own. */
static void open_menu(const agent *a)
{
	const char *self = getenv("AGENT_SIDEBAR_BIN");
	char selfbuf[512];

	if (self == NULL) {
		ssize_t r = readlink("/proc/self/exe", selfbuf,
				     sizeof selfbuf - 1);
		if (r <= 0)
			return;
		selfbuf[r] = '\0';
		self = selfbuf;
	}

	char title[128], jump[640], park[640], rename[900], killc[900];
	char newc[900], pid[32];
	snprintf(pid, sizeof pid, "%lld", a->pid);
	snprintf(title, sizeof title, " #[align=centre]%s ", a->sess);
	snprintf(jump, sizeof jump, "run-shell '%s --jump %s'", self, pid);
	snprintf(park, sizeof park, "run-shell '%s --park %s'", self, pid);
	/* start another agent in the same directory as this one */
	const char *kindlabel = a->agent[0] != '\0' ? a->agent : "claude";
	snprintf(newc, sizeof newc,
		 "command-prompt -p 'new %s agent in:' -I '%s' "
		 "\"run-shell '%s --new \\\"%%%%\\\" %s'\"",
		 kindlabel, a->cwd, self, kindlabel);

	/* tmux asks for confirmation itself, so there is no dialog to build */
	snprintf(killc, sizeof killc,
		 "confirm-before -p 'kill %s? (y/n)' "
		 "\"run-shell '%s --kill %s'\"",
		 a->sess, self, pid);

	/* the typed name is the only thing a shell sees, and it stays quoted */
	snprintf(rename, sizeof rename,
		 "command-prompt -p 'rename to:' -I '%s' "
		 "{ run-shell '%s --rename-session %s \"%%%%\"' }",
		 a->sess, self, pid);

	char *argv[] = {
		(char *)"tmux", (char *)"display-menu",
		(char *)"-T", title, (char *)"-x", (char *)"P",
		(char *)"-y", (char *)"P",
		(char *)"Jump to", (char *)"j", jump,
		(char *)(a->parked ? "Un-park" : "Park"), (char *)"p", park,
		(char *)"", (char *)"", (char *)"",
		(char *)"Rename", (char *)"r", rename,
		(char *)"New agent", (char *)"n", newc,
		(char *)"Kill", (char *)"k", killc,
		NULL,
	};

	trace("menu for sess=%s pid=%s", a->sess, pid);

	/* display-menu blocks while the menu is up when called from inside the
	   same client, so it must not be waited on. Double-fork rather than
	   ignoring SIGCHLD: that disposition is process-wide and permanent, and
	   it silently broke every later waitpid in run_tmux. */
	pid_t child = fork();
	if (child == 0) {
		pid_t grandchild = fork();
		if (grandchild == 0) {
			execvp("tmux", argv);
			_exit(127);
		}
		_exit(0); /* orphan it; init reaps the grandchild */
	}
	if (child > 0) {
		int st;
		while (waitpid(child, &st, 0) < 0 && errno == EINTR)
			; /* the intermediate child exits at once */
	}
}

static void trace(const char *fmt, ...)
{
	const char *path = getenv("AGENT_SIDEBAR_TRACE");
	if (path == NULL)
		return;

	FILE *fp = fopen(path, "a");
	if (fp == NULL)
		return;

	va_list ap;
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fputc('\n', fp);
	fclose(fp);
}

/* SGR mouse reports look like ESC [ < button ; col ; row M (press).
 *
 * A terminal is a byte stream, not a message stream: over SSH one report can
 * arrive split across two reads. An earlier version parsed whatever a single
 * read contained and discarded the rest, so a split report was silently lost
 * and the click had to be repeated. Unconsumed bytes are now carried over.
 */
static char in_buf[4096];
static size_t in_len;

static void act_on_click(const frame *f, const agent *a, int n, int button,
			 int row)
{
	if ((button & 32) != 0)  /* drag */
		return;
	if ((button & 64) != 0)  /* wheel */
		return;

	int which_button = button & 3;
	if (which_button > 2)
		return;

	int idx = row - 1;
	if (idx < 0 || idx >= f->rows)
		return;
	int which = f->row_agent[idx];
	trace("click button=%d row=%d -> agent %d", which_button, row, which);
	if (which < 0 || which >= n)
		return;

	if (which_button == 0)
		jump_to(&a[which]);
	else if (which_button == 1)
		open_menu(&a[which]);
	else
		agents_park_toggle(a[which].sess);
}

static void handle_input(const frame *f, const agent *a, int n)
{
	/* A full buffer would make the read below ask for zero bytes, return 0,
	   and take the early exit forever - clicks would stop for good. Nothing
	   this long can be a pending mouse report, so drop it. */
	if (in_len + 1 >= sizeof in_buf)
		in_len = 0;

	ssize_t r = read(STDIN_FILENO, in_buf + in_len,
			 sizeof in_buf - 1 - in_len);

	if (r <= 0)
		return;
	in_len += (size_t)r;

	size_t i = 0;
	while (i < in_len) {
		if (in_buf[i] != '\033') {
			i++;
			continue;
		}
		if (i + 2 >= in_len)
			break; /* too short to classify yet - keep it */
		if (in_buf[i + 1] != '[' || in_buf[i + 2] != '<') {
			i++;
			continue;
		}

		size_t j = i + 3;
		while (j < in_len && in_buf[j] != 'M' && in_buf[j] != 'm')
			j++;
		if (j >= in_len)
			break; /* terminator not here yet - keep from i */

		if (in_buf[j] == 'M') { /* press; releases are ignored */
			int button, col, row;
			char save = in_buf[j];
			in_buf[j] = '\0';
			if (sscanf(in_buf + i + 3, "%d;%d;%d", &button, &col,
				   &row) == 3) {
				(void)col;
				act_on_click(f, a, n, button, row);
			}
			in_buf[j] = save;
		}
		i = j + 1;
	}

	/* carry the tail; a report that never completes must not wedge us */
	if (i > 0 && i <= in_len) {
		memmove(in_buf, in_buf + i, in_len - i);
		in_len -= i;
	}
	if (in_len + 1 >= sizeof in_buf)
		in_len = 0;
}

/* Rewrites the park entry when a session is renamed, so a parked agent does
   not silently un-park because its key changed. */
/* Actions run as a separate short-lived process, so they reload state. */
static agent *agent_by_pid(long long pid)
{
	static agent list[AGENT_MAX];
	int n = agents_load(list, AGENT_MAX);

	for (int i = 0; i < n; i++)
		if (list[i].pid == pid)
			return &list[i];
	return NULL;
}

static void park_rename(const char *old, const char *new_name)
{
	/* Consult the park list itself: the session may have no running agent
	   yet still be parked, and a live-agent lookup would miss it. */
	if (!agents_park_has(old))
		return;
	agents_park_toggle(old);       /* drop the stale key */
	agents_park_toggle(new_name);  /* add the new one */
}

/* Re-exec when our own binary is replaced.
 *
 * A rebuilt binary does not reach a running process, so a fixed bug keeps
 * presenting as broken until someone restarts the pane by hand. The identity
 * compared is (inode, size, mtime) of the file we were started from - a
 * rebuild writes a new inode, so this is exact rather than heuristic.
 *
 * The change must be seen twice before acting: cc writes the output in place,
 * so a single sighting can be a half-written file.
 */
static char self_path[512];
static char *const *saved_argv;
static struct { ino_t ino; off_t size; time_t mtime; } self_id;

static int stat_self(struct stat *st)
{
	return self_path[0] != '\0' && stat(self_path, st) == 0;
}

static void self_record(void)
{
	ssize_t r = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
	struct stat st;

	if (r <= 0) {
		self_path[0] = '\0';
		return;
	}
	self_path[r] = '\0';

	if (stat_self(&st)) {
		self_id.ino = st.st_ino;
		self_id.size = st.st_size;
		self_id.mtime = st.st_mtime;
	}
}

static void self_reexec_if_replaced(void)
{
	static ino_t pending_ino;
	static off_t pending_size;
	static time_t pending_mtime;
	struct stat st;

	if (!stat_self(&st))
		return;
	if (st.st_ino == self_id.ino && st.st_size == self_id.size &&
	    st.st_mtime == self_id.mtime)
		return;

	/* seen once: remember it and wait for the next tick to confirm */
	if (st.st_ino != pending_ino || st.st_size != pending_size ||
	    st.st_mtime != pending_mtime) {
		pending_ino = st.st_ino;
		pending_size = st.st_size;
		pending_mtime = st.st_mtime;
		return;
	}
	if (st.st_size == 0 || access(self_path, X_OK) != 0)
		return;

	trace("re-exec: %s changed", self_path);
	tty_restore(); /* the successor sets the terminal up again */
	execv(self_path, saved_argv);
	/* only reached if exec failed; carry on with the old image */
	tty_setup();
	notify("agent-sidebar: could not restart after rebuild");
}

static void usage(void)
{
	fputs("agent-sidebar - live status of every Claude Code agent\n\n"
	      "  agent-sidebar            run in a tmux pane (live)\n"
	      "  agent-sidebar --once     print one frame and exit\n"
	      "  agent-sidebar --width N  force a width (with --once)\n"
	      "  agent-sidebar --rows N   force a height (with --once)\n",
	      stderr);
}

int main(int argc, char **argv)
{
	static agent agents[AGENT_MAX];
	static frame cur, prev;
	int once = 0;
	int force_width = 0;
	int force_rows = 0;
	int debug = 0;
	int list = 0;

	/* Actions, invoked by tmux menu items rather than by a person. */
	/* These take a pid, not a session name. A pid is always digits, so
	   nothing needs quoting when tmux menu items invoke them - a session
	   called "PR-3 BYO" broke every name-keyed action. */
	if (argc >= 3 && strcmp(argv[1], "--park") == 0) {
		agent *found = agent_by_pid(atoll(argv[2]));
		if (found != NULL)
			agents_park_toggle(found->sess);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "--new") == 0) {
		char dir[512];

		/* --list prints paths with $HOME collapsed, so whatever calls
		   this hands them back that way. */
		if (argv[2][0] == '~' &&
		    (argv[2][1] == '/' || argv[2][1] == '\0')) {
			const char *home = getenv("HOME");
			snprintf(dir, sizeof dir, "%s%s", home ? home : "",
				 argv[2] + 1);
		} else {
			snprintf(dir, sizeof dir, "%s", argv[2]);
		}

		struct stat st;
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
			notify("agent-sidebar: not a directory");
			return 2;
		}

		/* Launch whatever the row was running: an opencode row starts
		   opencode, anything else starts the default agent. Each is
		   overridable, e.g. @agent_sidebar_command_opencode. */
		const char *kind = (argc >= 4 && argv[3][0] != '\0')
				   ? argv[3] : "claude";

		char optname[96];
		snprintf(optname, sizeof optname,
			 "@agent_sidebar_command_%s", kind);

		char cmd[256] = "";
		char *opt[] = { (char *)"tmux", (char *)"show-option",
				(char *)"-gqv", optname, NULL };
		capture_all(opt, cmd, sizeof cmd);
		char *nl = strchr(cmd, '\n');
		if (nl != NULL)
			*nl = '\0';

		if (cmd[0] == '\0') {
			/* the older single-command option still wins if set */
			char *legacy[] = { (char *)"tmux",
					   (char *)"show-option", (char *)"-gqv",
					   (char *)"@agent_sidebar_new_command",
					   NULL };
			capture_all(legacy, cmd, sizeof cmd);
			nl = strchr(cmd, '\n');
			if (nl != NULL)
				*nl = '\0';
		}
		if (cmd[0] == '\0')
			snprintf(cmd, sizeof cmd, "%s", kind);

		/* name after the directory, suffixed until it is free */
		const char *base = strrchr(dir, '/');
		base = (base != NULL && base[1] != '\0') ? base + 1 : dir;

		char name[128];
		snprintf(name, sizeof name, "%.*s",
			 (int)(sizeof name - 5), base);
		for (int i = 2; i < 100; i++) {
			char *has[] = { (char *)"tmux", (char *)"has-session",
					(char *)"-t", (char *)name, NULL };
			if (run_tmux(has) != 0)
				break;
			snprintf(name, sizeof name, "%.*s-%d",
				 (int)(sizeof name - 8), base, i);
		}

		char *mk[] = { (char *)"tmux", (char *)"new-session",
			       (char *)"-d", (char *)"-s", name,
			       (char *)"-c", dir, cmd, NULL };
		if (run_tmux(mk) != 0) {
			notify("agent-sidebar: could not create the session");
			return 1;
		}

		/* move the client that asked to the session it just made */
		char client[128] = "";
		const char *self_pane = getenv("TMUX_PANE");
		if (self_pane != NULL && *self_pane != '\0') {
			char clients[2048];
			char *lc[] = { (char *)"tmux", (char *)"list-clients",
				       (char *)"-t", (char *)self_pane,
				       (char *)"-F", (char *)"#{client_name}",
				       NULL };
			if (capture_all(lc, clients, sizeof clients) == 0) {
				char *e = strchr(clients, '\n');
				if (e != NULL)
					*e = '\0';
				snprintf(client, sizeof client, "%.*s",
					 (int)(sizeof client - 1), clients);
			}
		}
		if (client[0] != '\0') {
			char *sw[] = { (char *)"tmux", (char *)"switch-client",
				       (char *)"-c", client, (char *)"-t",
				       name, NULL };
			run_tmux(sw);
		} else {
			char *sw[] = { (char *)"tmux", (char *)"switch-client",
				       (char *)"-t", name, NULL };
			run_tmux(sw);
		}
		trace("new session=%s dir=%s kind=%s cmd=%s", name, dir,
		      kind, cmd);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "--kill") == 0) {
		agent *found = agent_by_pid(atoll(argv[2]));
		if (found == NULL) {
			notify("agent-sidebar: no agent with that pid");
			return 2;
		}
		/* SIGTERM the agent, not its session: the pane and whatever
		   else lives in it survive, and the agent gets to exit
		   cleanly. */
		if (kill((pid_t)found->pid, SIGTERM) != 0) {
			notify("agent-sidebar: could not signal that agent");
			return 1;
		}
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "--jump") == 0) {
		agent *found = agent_by_pid(atoll(argv[2]));
		if (found != NULL)
			jump_to(found);
		return 0;
	}
	if (argc >= 4 && strcmp(argv[1], "--rename-session") == 0) {
		/* No fallback to argv[2] as a session name: a pid that matches
		   no agent was being handed to tmux as a target, which resolved
		   to an unrelated session and renamed it. Refuse instead. */
		agent *found = agent_by_pid(atoll(argv[2]));
		if (found == NULL || found->sess[0] == '\0') {
			notify("agent-sidebar: no agent with that pid");
			return 2;
		}
		const char *old = found->sess;
		if (argv[3][0] == '\0') {
			notify("agent-sidebar: rename needs a name");
			return 2;
		}
		char *rn[] = { (char *)"tmux", (char *)"rename-session",
			       (char *)"-t", (char *)old, (char *)"--",
			       argv[3], NULL };
		if (run_tmux(rn) != 0) {
			notify("agent-sidebar: rename failed");
			return 1;
		}
		park_rename(old, argv[3]);

		/* wake any running sidebar rather than making it wait out the
		   pane-map TTL */
		char pd[512];
		producer_dir(pd, sizeof pd);
		utimensat(AT_FDCWD, pd, NULL, 0);
		return 0;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--once") == 0) {
			once = 1;
		} else if (strcmp(argv[i], "--list") == 0) {
			list = 1;
		} else if (strcmp(argv[i], "--debug") == 0) {
			debug = 1;
		} else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			force_width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
			force_rows = atoi(argv[++i]);
		} else {
			usage();
			return 2;
		}
	}

	saved_argv = argv;
	self_record();

	int cols, rows;
	term_size(STDOUT_FILENO, &cols, &rows);
	if (force_width > 0)
		cols = force_width;

	if (list) {
		int n = agents_load(agents, AGENT_MAX);
		long long now = now_ms();
		for (int i = 0; i < n; i++) {
			const agent *a = &agents[i];
			long long secs = a->seen_ms > 0
					 ? (now - a->seen_ms) / 1000 : 0;
			char age[24];
			if (secs < 60)
				snprintf(age, sizeof age, "%llds", secs);
			else if (secs < 3600)
				snprintf(age, sizeof age, "%lldm", secs / 60);
			else if (secs < 86400)
				snprintf(age, sizeof age, "%lldh", secs / 3600);
			else
				snprintf(age, sizeof age, "%lldd", secs / 86400);

			/* hidden: pid, pane. shown: state, age, session, path */
			printf("%lld\t%s\t%s%-7s\033[0m\t%5s\t%-18s\t%s\t%s\n",
			       a->pid,
			       a->pane_id[0] ? a->pane_id : "-",
			       a->parked ? "\033[38;5;244m"
					 : status_colour_for(a->status),
			       a->parked ? "parked"
					 : agent_status_label(a->status),
			       age,
			       a->sess[0] ? a->sess : a->name,
			       a->cwd,
			       a->agent[0] != '\0' ? a->agent : "claude");
		}
		return 0;
	}

	if (debug) {
		int n = agents_load(agents, AGENT_MAX);
		printf("%-8s %-9s %-16s %-8s %s\n",
		       "pid", "status", "sess", "pane_id", "cwd");
		for (int i = 0; i < n; i++)
			printf("%-8lld %-9s %-16s %-8s %s\n", agents[i].pid,
			       agent_status_label(agents[i].status),
			       agents[i].sess[0] ? agents[i].sess : "-",
			       agents[i].pane_id[0] ? agents[i].pane_id : "-",
			       agents[i].cwd);
		return 0;
	}

	if (once) {
		int n = agents_load(agents, AGENT_MAX);
		if (n < 0) {
			fputs("agent-sidebar: cannot read the sessions "
			      "directory\n", stderr);
			return 1;
		}
		render_build(&cur, agents, n, cols,
			     force_rows > 0 ? force_rows : FRAME_ROWS,
			     now_ms(), self);
		for (int i = 0; i < cur.rows; i++)
			printf("%s\033[0m\n", cur.line[i]);
		return 0;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_winch;
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = on_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	char dir[512];
	sessions_dir(dir, sizeof dir);

	/* Watch both producer directories: the agent's own state, and the one
	   plugins write to. A rename touches the latter deliberately, which is
	   what makes a renamed session appear at once rather than on the TTL. */
	char pdir[512];
	producer_dir(pdir, sizeof pdir);

	int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	int watches = 0;
	if (ifd >= 0) {
		const uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO |
				      IN_CREATE | IN_DELETE | IN_MODIFY |
				      IN_ATTRIB;
		if (inotify_add_watch(ifd, dir, mask) >= 0)
			watches++;
		if (inotify_add_watch(ifd, pdir, mask) >= 0)
			watches++;
	}
	if (watches == 0 && ifd >= 0) {
		close(ifd);
		ifd = -1; /* fall back to the poll timeout alone */
	}

	trace("start: stdin isatty=%d TMUX_PANE=%s", isatty(STDIN_FILENO),
	      getenv("TMUX_PANE") ? getenv("TMUX_PANE") : "(unset)");
	atexit(tty_restore);
	emit("\033[?25l\033[2J");
	if (isatty(STDIN_FILENO))
		tty_setup();

	prev.rows = 0;
	int force = 1;

	while (!g_stop) {
		if (g_winch) {
			g_winch = 0;
			term_size(STDOUT_FILENO, &cols, &rows);
			force = 1;
		}

		self_reexec_if_replaced();

		int n = agents_load(agents, AGENT_MAX);
		if (n < 0)
			n = 0;
		char self[64];
		agents_self_session(self, sizeof self);
		render_build(&cur, agents, n, cols, rows, now_ms(), self);
		render_flush(STDOUT_FILENO, &cur, &prev, force);
		force = 0;

		struct pollfd pfd[2];
		int nfds = 0;
		int slot_inotify = -1, slot_stdin = -1;

		if (ifd >= 0) {
			pfd[nfds].fd = ifd;
			pfd[nfds].events = POLLIN;
			slot_inotify = nfds++;
		}
		if (isatty(STDIN_FILENO)) {
			pfd[nfds].fd = STDIN_FILENO;
			pfd[nfds].events = POLLIN;
			slot_stdin = nfds++;
		}

		int rc = poll(nfds > 0 ? pfd : NULL, (nfds_t)nfds, 1000);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rc > 0 && slot_inotify >= 0 &&
		    (pfd[slot_inotify].revents & POLLIN)) {
			char drain[4096];
			while (read(ifd, drain, sizeof drain) > 0)
				; /* the rebuild above re-reads the truth */
			agents_invalidate();
		}
		if (rc > 0 && slot_stdin >= 0 &&
		    (pfd[slot_stdin].revents & POLLIN))
			handle_input(&cur, agents, n);
	}

	tty_restore();
	return 0;
}
		char self[64];
		agents_self_session(self, sizeof self);

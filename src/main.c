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
		execvp("tmux", argv);
		_exit(127);
	}

	int st = 0;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Same, capturing the first line of stdout. */
static int capture_tmux(char *const argv[], char *out, size_t cap)
{
	int fds[2];

	if (cap == 0)
		return -1;
	out[0] = '\0';
	if (pipe(fds) != 0)
		return -1;

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
	for (;;) {
		ssize_t r = read(fds[0], out + got, cap - 1 - got);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		got += (size_t)r;
		if (got >= cap - 1)
			break;
	}
	close(fds[0]);
	out[got] = '\0';

	char *nl = strchr(out, '\n');
	if (nl != NULL)
		*nl = '\0';

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

static int safe_token(const char *s)
{
	if (*s == '\0')
		return 0;
	for (; *s != '\0'; s++) {
		if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
		    (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' ||
		    *s == '-' || *s == '%' || *s == '@' || *s == ':' ||
		    *s == '/')
			continue;
		return 0;
	}
	return 1;
}

static const char *my_session(void)
{
	static char cached[128];
	static int done;

	if (done)
		return cached;
	done = 1;

	const char *pane = getenv("TMUX_PANE");
	if (pane == NULL)
		return cached;

	char *argv[] = { (char *)"tmux", (char *)"display-message",
			 (char *)"-p", (char *)"-t", (char *)pane,
			 (char *)"#{session_name}", NULL };
	capture_tmux(argv, cached, sizeof cached);
	return cached;
}

/* Move the client that is looking at this sidebar to the clicked agent. The
   client is resolved from our own session rather than left to tmux's notion of
   "current", which is ambiguous with several clients attached. */
static void jump_to(const agent *a)
{
	if (a->pane_id[0] == '\0')
		return;

	const char *mine = my_session();
	char client[128] = "";

	if (mine[0] != '\0') {
		char *lc[] = { (char *)"tmux", (char *)"list-clients",
			       (char *)"-t", (char *)mine, (char *)"-F",
			       (char *)"#{client_name}", NULL };
		capture_tmux(lc, client, sizeof client);
	}

	if (client[0] != '\0') {
		char *sw[] = { (char *)"tmux", (char *)"switch-client",
			       (char *)"-c", client, (char *)"-t",
			       (char *)a->sess, NULL };
		run_tmux(sw);
	} else {
		char *sw[] = { (char *)"tmux", (char *)"switch-client",
			       (char *)"-t", (char *)a->sess, NULL };
		run_tmux(sw);
	}

	char *w[] = { (char *)"tmux", (char *)"select-window", (char *)"-t",
		      (char *)a->pane_id, NULL };
	char *pn[] = { (char *)"tmux", (char *)"select-pane", (char *)"-t",
		       (char *)a->pane_id, NULL };
	run_tmux(w);
	run_tmux(pn);

	trace("jump sess=%s pane=%s client=%s", a->sess, a->pane_id, client);
}

/* tmux renders the menu, handles its keys and runs the chosen command, so the
   sidebar needs no menu widget of its own. */
static void open_menu(const agent *a)
{
	if (!safe_token(a->sess))
		return;

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

	char cmd[4096];
	snprintf(cmd, sizeof cmd,
		 "tmux display-menu -T ' #[align=centre]%s ' -x P -y P "
		 "'Jump to' j \"run-shell '%s --jump %s'\" "
		 "'%s' p \"run-shell '%s --park %s'\" "
		 "'' "
		 "'Rename' r \"command-prompt -p 'rename to:' -I '%s' "
		 "{ run-shell '%s --rename-session %s \\\"%%%%\\\"' }\" "
		 "2>/dev/null &",  /* backgrounded: display-menu blocks while the
				      menu is up, and the render loop must not */
		 a->sess, self, a->sess,
		 a->parked ? "Un-park" : "Park",
		 self, a->sess,
		 a->sess, self, a->sess);

	trace("menu cmd: %s", cmd);
	int rc = system(cmd);
	trace("menu rc=%d", rc);
}

/* SGR mouse reports look like ESC [ < button ; col ; row M (press). */
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

static void handle_input(const frame *f, const agent *a, int n)
{
	char buf[1024];
	ssize_t r = read(STDIN_FILENO, buf, sizeof buf - 1);

	if (r <= 0) {
		trace("read returned %zd", r);
		return;
	}
	buf[r] = '\0';

	{
		char hex[256];
		size_t o = 0;
		for (ssize_t k = 0; k < r && o + 4 < sizeof hex; k++)
			o += (size_t)snprintf(hex + o, sizeof hex - o, "%02x ",
					      (unsigned char)buf[k]);
		trace("stdin %zd bytes: %s", r, hex);
	}

	for (ssize_t i = 0; i + 3 < r; i++) {
		if (buf[i] != '\033' || buf[i + 1] != '[' || buf[i + 2] != '<')
			continue;

		int button, col, row;
		char kind;
		if (sscanf(buf + i + 3, "%d;%d;%d%c", &button, &col, &row,
			   &kind) != 4)
			continue;
		(void)col;

		if (kind != 'M')      /* release */
			continue;
		if ((button & 32) != 0) /* drag */
			continue;
		if ((button & 64) != 0) /* wheel */
			continue;
		int which_button = button & 3;
		if (which_button > 2)
			continue; /* left jumps; middle and right both park */

		int idx = row - 1;
		if (idx < 0 || idx >= f->rows)
			continue;
		int which = f->row_agent[idx];
		trace("click button=%d row=%d -> agent %d", which_button, row,
		      which);
		if (which < 0 || which >= n)
			return;

		if (which_button == 0) {
			trace("jump to sess=%s pane=%s", a[which].sess,
			      a[which].pane_id);
			jump_to(&a[which]);
		} else if (which_button == 1) {
			trace("menu for sess=%s", a[which].sess);
			open_menu(&a[which]);
		} else {
			trace("park toggle sess=%s", a[which].sess);
			agents_park_toggle(a[which].sess);
		}
		return;
	}
}

/* Rewrites the park entry when a session is renamed, so a parked agent does
   not silently un-park because its key changed. */
static void park_rename(const char *old, const char *new_name)
{
	/* Consult the park list itself: the session may have no running agent
	   yet still be parked, and a live-agent lookup would miss it. */
	if (!agents_park_has(old))
		return;
	agents_park_toggle(old);       /* drop the stale key */
	agents_park_toggle(new_name);  /* add the new one */
}

static void usage(void)
{
	fputs("agent-sidebar - live status of every Claude Code agent\n\n"
	      "  agent-sidebar            run in a tmux pane (live)\n"
	      "  agent-sidebar --once     print one frame and exit\n"
	      "  agent-sidebar --width N  force a width (with --once)\n",
	      stderr);
}

int main(int argc, char **argv)
{
	static agent agents[AGENT_MAX];
	static frame cur, prev;
	int once = 0;
	int force_width = 0;
	int debug = 0;

	/* Actions, invoked by tmux menu items rather than by a person. */
	if (argc >= 3 && strcmp(argv[1], "--park") == 0) {
		agents_park_toggle(argv[2]);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "--jump") == 0) {
		agent list[AGENT_MAX];
		int n = agents_load(list, AGENT_MAX);
		for (int i = 0; i < n; i++)
			if (strcmp(list[i].sess, argv[2]) == 0) {
				jump_to(&list[i]);
				break;
			}
		return 0;
	}
	if (argc >= 4 && strcmp(argv[1], "--rename-session") == 0) {
		if (argv[3][0] == '\0') {
			notify("agent-sidebar: rename needs a name");
			return 2;
		}
		char *rn[] = { (char *)"tmux", (char *)"rename-session",
			       (char *)"-t", argv[2], (char *)"--", argv[3],
			       NULL };
		if (run_tmux(rn) != 0) {
			notify("agent-sidebar: rename failed");
			return 1;
		}
		park_rename(argv[2], argv[3]);

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
		} else if (strcmp(argv[i], "--debug") == 0) {
			debug = 1;
		} else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			force_width = atoi(argv[++i]);
		} else {
			usage();
			return 2;
		}
	}

	int cols, rows;
	term_size(STDOUT_FILENO, &cols, &rows);
	if (force_width > 0)
		cols = force_width;

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
		render_build(&cur, agents, n, cols, FRAME_ROWS, now_ms());
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

		int n = agents_load(agents, AGENT_MAX);
		if (n < 0)
			n = 0;
		render_build(&cur, agents, n, cols, rows, now_ms());
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

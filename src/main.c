#define _POSIX_C_SOURCE 200809L

#include "agents.h"
#include "render.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
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

/* Everything interpolated into the shell command below must pass this first.
   tmux session names are user-chosen, so this is the boundary. */
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
	if (pane == NULL || !safe_token(pane))
		return cached;

	char cmd[256];
	snprintf(cmd, sizeof cmd,
		 "tmux display-message -p -t '%s' '#{session_name}' 2>/dev/null",
		 pane);

	FILE *fp = popen(cmd, "r");
	if (fp == NULL)
		return cached;
	if (fgets(cached, sizeof cached, fp) != NULL) {
		char *nl = strchr(cached, '\n');
		if (nl != NULL)
			*nl = '\0';
	}
	pclose(fp);
	return cached;
}

/* Move the client that is looking at this sidebar to the clicked agent. The
   client is resolved from our own session rather than left to tmux's notion of
   "current", which is ambiguous with several clients attached. */
static void jump_to(const agent *a)
{
	if (!safe_token(a->pane_id) || !safe_token(a->sess))
		return;

	const char *mine = my_session();
	char cmd[1024];

	if (safe_token(mine))
		snprintf(cmd, sizeof cmd,
			 "c=$(tmux list-clients -t '%s' -F '#{client_name}' "
			 "2>/dev/null | head -1); "
			 "[ -n \"$c\" ] && tmux switch-client -c \"$c\" "
			 "-t '%s' 2>/dev/null; "
			 "tmux select-window -t '%s' 2>/dev/null; "
			 "tmux select-pane -t '%s' 2>/dev/null",
			 mine, a->sess, a->pane_id, a->pane_id);
	else
		snprintf(cmd, sizeof cmd,
			 "tmux switch-client -t '%s' 2>/dev/null; "
			 "tmux select-window -t '%s' 2>/dev/null; "
			 "tmux select-pane -t '%s' 2>/dev/null",
			 a->sess, a->pane_id, a->pane_id);

	trace("jump cmd: %s", cmd);
	int rc = system(cmd);
	trace("jump rc=%d", rc);
}

/* SGR mouse reports look like ESC [ < button ; col ; row M (press). */
static void trace(const char *fmt, ...)
{
	const char *path = getenv("CLAUDE_SIDEBAR_TRACE");
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
		} else {
			trace("park toggle sess=%s", a[which].sess);
			agents_park_toggle(a[which].sess);
		}
		return;
	}
}

static void usage(void)
{
	fputs("claude-sidebar - live status of every Claude Code agent\n\n"
	      "  claude-sidebar            run in a tmux pane (live)\n"
	      "  claude-sidebar --once     print one frame and exit\n"
	      "  claude-sidebar --width N  force a width (with --once)\n",
	      stderr);
}

int main(int argc, char **argv)
{
	static agent agents[AGENT_MAX];
	static frame cur, prev;
	int once = 0;
	int force_width = 0;
	int debug = 0;

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
			fputs("claude-sidebar: cannot read the sessions "
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

	int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	int wd = -1;
	if (ifd >= 0)
		wd = inotify_add_watch(ifd, dir,
				       IN_CLOSE_WRITE | IN_MOVED_TO |
				       IN_CREATE | IN_DELETE | IN_MODIFY);
	if (wd < 0 && ifd >= 0) {
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
		}
		if (rc > 0 && slot_stdin >= 0 &&
		    (pfd[slot_stdin].revents & POLLIN))
			handle_input(&cur, agents, n);
	}

	tty_restore();
	return 0;
}

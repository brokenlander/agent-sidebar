#define _POSIX_C_SOURCE 200809L

#include "agents.h"
#include "render.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
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

static void cursor_show(void)
{
	const char *s = "\033[?25h";
	ssize_t r = write(STDOUT_FILENO, s, strlen(s));
	(void)r;
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

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--once") == 0) {
			once = 1;
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

	atexit(cursor_show);
	{
		const char *init = "\033[?25l\033[2J";
		ssize_t r = write(STDOUT_FILENO, init, strlen(init));
		(void)r;
	}

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

		struct pollfd pfd = { .fd = ifd, .events = POLLIN };
		int nfds = ifd >= 0 ? 1 : 0;
		int rc = poll(nfds > 0 ? &pfd : NULL, (nfds_t)nfds, 1000);

		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rc > 0 && (pfd.revents & POLLIN)) {
			char buf[4096];
			while (read(ifd, buf, sizeof buf) > 0)
				; /* drain; the rebuild below reads the truth */
		}
	}

	cursor_show();
	return 0;
}

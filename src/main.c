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

static long long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The state directory is shared (pm-board writes there), so only our own
   files there are worth a refresh; the map's lock and temp file are not. */
static int events_matter(int ifd, int state_wd)
{
	const char *map = strrchr(agents_map_path(), '/');
	map = map != NULL ? map + 1 : agents_map_path();

	_Alignas(struct inotify_event) char buf[4096];
	int matter = 0;
	ssize_t len;

	while ((len = read(ifd, buf, sizeof buf)) > 0) {
		for (char *p = buf; p < buf + len;) {
			const struct inotify_event *ev = (const void *)p;
			const char *name = ev->len > 0 ? ev->name : "";
			size_t n = strlen(name);
			if (ev->len == 0)
				matter = 1;
			else if (ev->wd != state_wd)
				matter |= n > 5 && strcmp(name + n - 5, ".json") == 0;
			else
				matter |= strcmp(name, "parked") == 0 ||
					  strcmp(name, "frozen") == 0 ||
					  strcmp(name, map) == 0;
			p += sizeof *ev + ev->len;
		}
	}
	return matter;
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

/* Registered with atexit, so however the loop ends the pane is never left in
   raw mode with mouse reporting on. */
static void tty_restore(void)
{
	emit("\033[?1006l\033[?1000l\033[?25h\033[?1049l");
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

/* The pty is told every size tmux passes through, even one the width hook has already undone. */
static void pane_size(int *cols, int *rows)
{
	const char *pane = getenv("TMUX_PANE");
	char out[32];
	int c, r;

	if (pane != NULL && *pane != '\0') {
		char *argv[] = { (char *)"tmux", (char *)"display-message",
				 (char *)"-p", (char *)"-t", (char *)pane,
				 (char *)"#{pane_width} #{pane_height}", NULL };
		if (tmux_capture(argv, out, sizeof out) == 0 &&
		    sscanf(out, "%d %d", &c, &r) == 2 && c > 0 && r > 0) {
			*cols = c;
			*rows = r;
			return;
		}
	}
	term_size(STDOUT_FILENO, cols, rows);
}

/* Failures used to be swallowed by 2>/dev/null, which made a rejected rename
   look like a crash. Say so on the status line instead. */
static void notify(const char *msg)
{
	char *argv[] = { (char *)"tmux", (char *)"display-message",
			 (char *)"--", (char *)msg, NULL };
	run_tmux(argv);
}

/* The client viewing this sidebar, rather than tmux's notion of "current",
   which is ambiguous with several clients attached. Keyed on our own pane id,
   never the session name - a rename left the cached name pointing at nothing,
   the client came back empty, and the switch fell through to whichever client
   tmux considered current, usually a different terminal. One ~4ms call. */
static void resolve_client(char *client, size_t cap)
{
	client[0] = '\0';
	const char *self_pane = getenv("TMUX_PANE");
	if (self_pane == NULL || *self_pane == '\0')
		return;

	char list[2048];
	char *lc[] = { (char *)"tmux", (char *)"list-clients",
		       (char *)"-t", (char *)self_pane, (char *)"-F",
		       (char *)"#{client_activity} #{client_name}", NULL };
	if (tmux_capture(lc, list, sizeof list) != 0)
		return;

	/* Two terminals can view one session. Prefer the most recently used -
	   the one in front of whoever clicked. */
	long long best = -1;
	char *save = NULL;
	for (char *line = strtok_r(list, "\n", &save); line != NULL;
	     line = strtok_r(NULL, "\n", &save)) {
		char *sp = strchr(line, ' ');
		if (sp == NULL)
			continue;
		*sp = '\0';
		long long act = atoll(line);
		if (act > best) {
			best = act;
			snprintf(client, cap, "%s", sp + 1);
		}
	}
}

/* By id only ("$3", or "%27" which selects its window too): tmux reads the
   name "v1.2" as session v1, pane 2. */
static void jump_client(const char *target)
{
	char client[128];

	if (target == NULL || target[0] == '\0')
		return;
	resolve_client(client, sizeof client);

	char *with[] = { (char *)"tmux", (char *)"switch-client", (char *)"-c",
			 client, (char *)"-t", (char *)target, NULL };
	char *no[] = { (char *)"tmux", (char *)"switch-client", (char *)"-t",
		       (char *)target, NULL };
	int rc = run_tmux(client[0] != '\0' ? with : no);
	trace("jump target=%s client=%s rc=%d", target, client, rc);
}

/* -M (let the menu handle mouse clicks) landed in tmux 3.5; before that the
   flag is rejected and the menu will not open at all, so it is gated on the
   server's own version. Cached: a server cannot change version under a running
   pane without restarting, which restarts this too. */
static int menu_mouse_ok(void)
{
	static int cached = -1;
	char out[32];

	if (cached != -1)
		return cached;
	cached = 0;
	char *argv[] = { (char *)"tmux", (char *)"display-message",
			 (char *)"-p", (char *)"#{version}", NULL };
	if (tmux_capture(argv, out, sizeof out) == 0) {
		int maj = 0, min = 0;
		if (sscanf(out, "%d.%d", &maj, &min) == 2)
			cached = maj > 3 || (maj == 3 && min >= 5);
	}
	return cached;
}

/* tmux expands $NAME and ~ in double quotes and '#' in formats (prompts,
   titles, menu items, session names). -1 rather than truncate a quote. */
static int fmt_escape(char *dst, size_t cap, const char *src)
{
	size_t o = 0;

	for (; *src != '\0'; src++) {
		if (o + 3 > cap)
			return -1;
		if (*src == '#')
			dst[o++] = '#';
		dst[o++] = *src;
	}
	dst[o] = '\0';
	return 0;
}

static int tm_quote(char *dst, size_t cap, const char *src)
{
	size_t o = 0;

	if (cap < 3)
		return -1;
	dst[o++] = '"';
	for (; *src != '\0'; src++) {
		if (o + 4 > cap)
			return -1;
		if (strchr("\\\"$~", *src) != NULL)
			dst[o++] = '\\';
		dst[o++] = *src;
	}
	dst[o++] = '"';
	dst[o] = '\0';
	return 0;
}

static int sh_quote(char *dst, size_t cap, const char *src)
{
	size_t o = 0;

	if (cap < 3)
		return -1;
	dst[o++] = '\'';
	for (; *src != '\0'; src++) {
		if (o + 6 > cap)
			return -1;
		if (*src == '\'') {
			memcpy(dst + o, "'\\''", 4);
			o += 4;
		} else {
			dst[o++] = *src;
		}
	}
	dst[o++] = '\'';
	dst[o] = '\0';
	return 0;
}

/* tm_quote(fmt_escape(src)): a format argument inside a command string. */
static int tm_format(char *dst, size_t cap, const char *src)
{
	char esc[1024];

	return fmt_escape(esc, sizeof esc, src) == 0 ? tm_quote(dst, cap, esc) : -1;
}

/* The binary's own path, for menu items that run it. */
static const char *self_exe(void)
{
	static char buf[512];
	const char *env = getenv("AGENT_SIDEBAR_BIN");

	if (env != NULL)
		return env;
	if (buf[0] == '\0') {
		ssize_t r = readlink("/proc/self/exe", buf, sizeof buf - 1);
		if (r <= 0)
			return NULL;
		buf[r] = '\0';
	}
	return buf;
}

/* `run-shell "'<self>' <args>"`; args must already be safe for sh. */
static int run_self(char *dst, size_t cap, const char *args)
{
	const char *self = self_exe();
	char q[1100], sh[1400];

	if (self == NULL || sh_quote(q, sizeof q, self) != 0)
		return -1;
	if (snprintf(sh, sizeof sh, "%s %s", q, args) >= (int)sizeof sh)
		return -1;
	if (snprintf(dst, cap, "run-shell ") >= (int)cap)
		return -1;
	return tm_format(dst + 10, cap - 10, sh);
}

/* "%%%" is the typed text with quotes and '$' escaped: it reaches the option
   verbatim and no shell ever sees it (take_input reads it back). */
static int prompt_self(char *dst, size_t cap, const char *args)
{
	char run[1700];

	if (run_self(run, sizeof run, args) != 0)
		return -1;
	return snprintf(dst, cap, "set -g @agent_sidebar_input \"%%%%%%\" ; %s",
			run) >= (int)cap ? -1 : 0;
}

/* -O keeps a menu up when a click lands off an item instead of closing it,
   and -M lets a click choose one. A process-opened menu is marked "no mouse"
   by tmux, so without these the button release that opened it would close it
   again. -M needs tmux 3.5. */
static int menu_head(char **argv, const char *title)
{
	int k = 0;
	argv[k++] = (char *)"tmux";
	argv[k++] = (char *)"display-menu";
	argv[k++] = (char *)"-O";
	if (menu_mouse_ok())
		argv[k++] = (char *)"-M";
	argv[k++] = (char *)"-T";
	argv[k++] = (char *)title;
	argv[k++] = (char *)"-x";
	argv[k++] = (char *)"P";
	argv[k++] = (char *)"-y";
	argv[k++] = (char *)"P";
	return k;
}

/* display-menu blocks while the menu is up when called from inside the same
   client, so it must not be waited on. Double-fork rather than ignoring
   SIGCHLD: that disposition is process-wide and permanent, and it silently
   broke every later waitpid in run_tmux. */
static void spawn_menu(char *const argv[])
{
	pid_t child = fork();
	if (child == 0) {
		if (fork() == 0) {
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

/* tmux renders the menu, handles its keys and runs the chosen command, so the
   sidebar needs no menu widget of its own. */
static void open_menu(const agent *a)
{
	char title[200], esc[140], args[160], text[600], cmd[4000];
	char jump[1800], park[1800], freeze[1800], rename[6000];
	char newc[8000], killc[4000], kind[64], qkind[80];
	int bad = 0;

	snprintf(kind, sizeof kind, "%s", a->agent[0] != '\0' ? a->agent : "claude");
	bad |= sh_quote(qkind, sizeof qkind, kind);
	bad |= fmt_escape(esc, sizeof esc, a->sess);
	snprintf(title, sizeof title, "#[align=centre] %s ", esc);

	snprintf(args, sizeof args, "--jump %lld", a->pid);
	bad |= run_self(jump, sizeof jump, args);
	snprintf(args, sizeof args, "--park %lld", a->pid);
	bad |= run_self(park, sizeof park, args);
	snprintf(args, sizeof args, "--freeze %lld", a->pid);
	bad |= run_self(freeze, sizeof freeze, args);

	/* start another agent of the same kind, in the same directory */
	char qp[700], qi[1100], qc[3000];
	snprintf(args, sizeof args, "--new - %s", qkind);
	bad |= prompt_self(cmd, sizeof cmd, args);
	snprintf(text, sizeof text, "new %s agent in:", kind);
	bad |= tm_format(qp, sizeof qp, text);
	bad |= tm_format(qi, sizeof qi, a->cwd);
	bad |= tm_quote(qc, sizeof qc, cmd);
	if (snprintf(newc, sizeof newc, "command-prompt -p %s -I %s %s", qp, qi,
		     qc) >= (int)sizeof newc)
		bad = -1;

	/* tmux asks for confirmation itself, so there is no dialog to build */
	snprintf(args, sizeof args, "--kill %lld", a->pid);
	bad |= run_self(cmd, sizeof cmd, args);
	snprintf(text, sizeof text, "kill %s? (y/n)", a->sess);
	bad |= tm_format(qp, sizeof qp, text);
	bad |= tm_quote(qc, sizeof qc, cmd);
	if (snprintf(killc, sizeof killc, "confirm-before -p %s %s", qp, qc) >=
	    (int)sizeof killc)
		bad = -1;

	/* No -I: pre-filling the current name means clearing it before you can
	   type, and renaming to a variation of the old name is the rare case.
	   The prompt carries the context instead. Enter on an empty field is
	   refused by --rename-session, not acted on. */
	snprintf(args, sizeof args, "--rename-session %lld -", a->pid);
	bad |= prompt_self(cmd, sizeof cmd, args);
	snprintf(text, sizeof text, "rename %s to:", a->sess);
	bad |= tm_format(qp, sizeof qp, text);
	bad |= tm_quote(qc, sizeof qc, cmd);
	if (snprintf(rename, sizeof rename, "command-prompt -p %s %s", qp, qc) >=
	    (int)sizeof rename)
		bad = -1;

	if (bad != 0) {
		notify("agent-sidebar: that name is too long for a menu");
		return;
	}

	char *argv[40];
	int k = menu_head(argv, title);
	argv[k++] = (char *)"Jump to";
	argv[k++] = (char *)"j";
	argv[k++] = jump;
	argv[k++] = (char *)(a->frozen ? "Un-freeze" : "Freeze");
	argv[k++] = (char *)"f";
	argv[k++] = freeze;
	argv[k++] = (char *)(a->parked ? "Un-park" : "Park");
	argv[k++] = (char *)"p";
	argv[k++] = park;
	argv[k++] = (char *)"";
	argv[k++] = (char *)"";
	argv[k++] = (char *)"";
	argv[k++] = (char *)"Rename";
	argv[k++] = (char *)"r";
	argv[k++] = rename;
	argv[k++] = (char *)"New agent";
	argv[k++] = (char *)"n";
	argv[k++] = newc;
	argv[k++] = (char *)"Kill";
	argv[k++] = (char *)"k";
	argv[k++] = killc;
	argv[k++] = NULL;

	trace("menu for sess=%s pid=%lld", a->sess, a->pid);
	spawn_menu(argv);
}

/* Middle-click on a non-agent session row: jump to it or kill it, by id. */
static void open_session_menu(const char *name, const char *id)
{
	char title[200], esc[140], text[200], qp[600], qid[40];
	char jump[64], inner[96], qin[200], killc[900];
	int bad = 0;

	bad |= fmt_escape(esc, sizeof esc, name);
	snprintf(title, sizeof title, "#[align=centre] %s ", esc);
	bad |= tm_quote(qid, sizeof qid, id);
	snprintf(jump, sizeof jump, "switch-client -t %s", qid);
	snprintf(inner, sizeof inner, "kill-session -t %s", qid);
	bad |= tm_quote(qin, sizeof qin, inner);
	snprintf(text, sizeof text, "kill %s? (y/n)", name);
	bad |= tm_format(qp, sizeof qp, text);
	snprintf(killc, sizeof killc, "confirm-before -p %s %s", qp, qin);
	if (bad != 0)
		return;

	char *argv[16];
	int k = menu_head(argv, title);
	argv[k++] = (char *)"Jump to";
	argv[k++] = (char *)"j";
	argv[k++] = jump;
	argv[k++] = (char *)"Kill";
	argv[k++] = (char *)"k";
	argv[k++] = killc;
	argv[k++] = NULL;
	spawn_menu(argv);
}

static char g_legend[1024];

/* @agent_sidebar_legend ("key label;..."; ';' so a key can be '|') as a menu
   that only shows them. */
static void open_keys_menu(void)
{
	char *argv[200];
	char items[2048];
	size_t o = 0;
	int k = menu_head(argv, "#[align=centre] #{prefix} + key ");

	for (const char *p = g_legend; *p != '\0'; p += *p == ';') {
		size_t len = strcspn(p, ";");
		const char *sp = memchr(p, ' ', len);
		const char *label = sp != NULL ? sp + 1 : p;
		size_t klen = sp != NULL ? (size_t)(sp - p) : 0;
		size_t llen = len - (size_t)(label - p);

		if (k + 4 > (int)(sizeof argv / sizeof argv[0]) ||
		    o + klen + 2 * llen + 2 > sizeof items)
			break;
		argv[k++] = items + o;
		for (size_t i = 0; i < llen; i++) {
			if (label[i] == '#') /* a label is a format */
				items[o++] = '#';
			items[o++] = label[i];
		}
		items[o++] = '\0';
		argv[k++] = memcpy(items + o, p, klen);
		o += klen;
		items[o++] = '\0';
		argv[k++] = (char *)"";
		p += len;
	}
	argv[k++] = NULL;
	trace("shortcuts menu");
	spawn_menu(argv);
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
	if (idx == f->keys_row) {
		trace("click button=%d row=%d -> shortcuts", which_button, row);
		if (which_button != 2)
			open_keys_menu();
		return;
	}
	int which = f->row_agent[idx];
	if (which >= 0 && which < n) {
		trace("click button=%d row=%d -> agent %d", which_button, row,
		      which);
		if (which_button == 0)
			jump_client(a[which].pane_id);
		else if (which_button == 1)
			open_menu(&a[which]);
		else
			agents_park_toggle(a[which].sess);
		return;
	}

	if (f->row_sid[idx][0] != '\0') { /* a non-agent session row */
		trace("click button=%d row=%d -> session %s", which_button, row,
		      f->row_session[idx]);
		if (which_button == 0)
			jump_client(f->row_sid[idx]);
		else if (which_button == 1)
			open_session_menu(f->row_session[idx], f->row_sid[idx]);
	}
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

/* Rewrites the park and freeze entries when a session is renamed, so a
   listed agent does not silently drop off because its key changed. */
static void lists_rename(const char *old, const char *new_name)
{
	/* Consult the lists themselves: the session may have no running agent
	   yet still be listed, and a live-agent lookup would miss it. */
	if (agents_park_has(old)) {
		agents_park_toggle(old);       /* drop the stale key */
		agents_park_toggle(new_name);  /* add the new one */
	}
	if (agents_freeze_has(old)) {
		agents_freeze_toggle(old);
		agents_freeze_toggle(new_name);
	}
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
	fputs("agent-sidebar - live status of every coding agent\n\n"
	      "  agent-sidebar            run in a tmux pane (live)\n"
	      "  agent-sidebar --once     print one frame and exit\n"
	      "  agent-sidebar --width N  force a width (with --once)\n"
	      "  agent-sidebar --rows N   force a height (with --once)\n"
	      "  agent-sidebar --list     one agent per line, for the picker\n"
	      "  agent-sidebar --debug    dump the resolved rows\n",
	      stderr);
}

/* What prompt_self's prompt saved, cleared once read. */
static void take_input(char *dst, size_t cap)
{
	char *get[] = { (char *)"tmux", (char *)"show-option", (char *)"-gqv",
			(char *)"@agent_sidebar_input", NULL };
	char *unset[] = { (char *)"tmux", (char *)"set-option", (char *)"-gu",
			  (char *)"@agent_sidebar_input", NULL };

	if (tmux_capture(get, dst, cap) != 0)
		dst[0] = '\0';
	dst[strcspn(dst, "\n")] = '\0';
	run_tmux(unset);
}

/* A new session named for the directory, running the row's kind of agent
   (@agent_sidebar_command_<kind> overrides), and the asking client moved to it. */
static int new_agent(const char *where, const char *kind)
{
	char given[512], dir[512];

	if (strcmp(where, "-") == 0)
		take_input(given, sizeof given);
	else
		snprintf(given, sizeof given, "%s", where);

	/* --list and the menu offer paths with $HOME collapsed */
	if (given[0] == '~' && (given[1] == '/' || given[1] == '\0')) {
		const char *home = getenv("HOME");
		snprintf(dir, sizeof dir, "%s%s", home ? home : "", given + 1);
	} else {
		snprintf(dir, sizeof dir, "%s", given);
	}

	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		notify("agent-sidebar: not a directory");
		return 2;
	}
	if (kind[0] == '\0')
		kind = "claude";

	char optname[96], cmd[256] = "";
	snprintf(optname, sizeof optname, "@agent_sidebar_command_%s", kind);
	char *opt[] = { (char *)"tmux", (char *)"show-option", (char *)"-gqv",
			optname, NULL };
	tmux_capture(opt, cmd, sizeof cmd);
	cmd[strcspn(cmd, "\n")] = '\0';
	if (cmd[0] == '\0') {
		/* the older single-command option still wins if set */
		char *legacy[] = { (char *)"tmux", (char *)"show-option",
				   (char *)"-gqv",
				   (char *)"@agent_sidebar_new_command", NULL };
		tmux_capture(legacy, cmd, sizeof cmd);
		cmd[strcspn(cmd, "\n")] = '\0';
	}
	if (cmd[0] == '\0')
		snprintf(cmd, sizeof cmd, "%s", kind);

	/* name after the directory, suffixed until it is free */
	const char *base = strrchr(dir, '/');
	base = (base != NULL && base[1] != '\0') ? base + 1 : dir;
	char name[128], exact[132], esc[260], id[32];
	snprintf(name, sizeof name, "%.*s", (int)(sizeof name - 5), base);
	for (int i = 2; i < 100; i++) {
		snprintf(exact, sizeof exact, "=%s", name);
		char *has[] = { (char *)"tmux", (char *)"has-session",
				(char *)"-t", exact, NULL };
		if (run_tmux(has) != 0)
			break;
		snprintf(name, sizeof name, "%.*s-%d", (int)(sizeof name - 8),
			 base, i);
	}
	fmt_escape(esc, sizeof esc, name);

	char *mk[] = { (char *)"tmux", (char *)"new-session", (char *)"-d",
		       (char *)"-P", (char *)"-F", (char *)"#{session_id}",
		       (char *)"-s", esc, (char *)"-c", dir, cmd, NULL };
	if (tmux_capture(mk, id, sizeof id) != 0) {
		notify("agent-sidebar: could not create the session");
		return 1;
	}
	id[strcspn(id, "\n")] = '\0';
	jump_client(id);
	trace("new session=%s dir=%s kind=%s cmd=%s", name, dir, kind, cmd);
	return 0;
}

static char g_exclude[256];
static int g_show_sessions;

/* Read the options once, in one tmux call. They change only on a config
   reload, which does not reach a running process, so there is nothing to
   poll. */
static void read_config(void)
{
	char buf[1024];
	char *argv[] = { (char *)"tmux", (char *)"display-message", (char *)"-p",
			 (char *)"#{@agent_sidebar_legend}\t"
				 "#{@agent_sidebar_sessions}\t"
				 "#{@agent_sidebar_exclude}\t"
				 "#{@agent_sidebar_idle_wait}",
			 NULL };
	char *f[4] = { buf, NULL, NULL, NULL };

	if (tmux_capture(argv, buf, sizeof buf) != 0)
		return;
	buf[strcspn(buf, "\n")] = '\0';
	for (int i = 1; i < 4; i++) {
		f[i] = strchr(f[i - 1], '\t');
		if (f[i] == NULL)
			return;
		*f[i]++ = '\0';
	}
	snprintf(g_legend, sizeof g_legend, "%s", f[0]);
	g_show_sessions = strcmp(f[1], "on") == 0 || strcmp(f[1], "1") == 0 ||
			  strcmp(f[1], "yes") == 0 || strcmp(f[1], "true") == 0;
	snprintf(g_exclude, sizeof g_exclude, "%s", f[2]);

	/* Minutes of idleness after which an agent reads as wanting you. 0 turns
	   the promotion off and leaves idle permanently green. */
	char *end = NULL;
	long mins = strtol(f[3], &end, 10);
	if (end != f[3] && mins >= 0)
		agents_set_idle_wait((long long)mins * 60);
}

/* The menu actions take a pid, never a session name: digits need no quoting.
   A typed directory or name arrives as "-", meaning take_input. */
static int action(int argc, char **argv)
{
	const char *verb = argv[1];
	agent *found = argc >= 3 ? agent_by_pid(atoll(argv[2])) : NULL;

	if (strcmp(verb, "--panes") == 0) {
		agents_refresh_panes();
		return 0;
	}
	if (argc < 3)
		return -1;
	if (strcmp(verb, "--park") == 0) {
		if (found != NULL)
			agents_park_toggle(found->sess);
		return 0;
	}
	if (strcmp(verb, "--freeze") == 0) {
		if (found != NULL)
			agents_freeze_toggle(found->sess);
		return 0;
	}
	if (strcmp(verb, "--jump") == 0) {
		if (found != NULL)
			jump_client(found->pane_id);
		return 0;
	}
	if (strcmp(verb, "--kill") == 0) {
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
	if (strcmp(verb, "--new") == 0)
		return new_agent(argv[2], argc >= 4 ? argv[3] : "");
	if (argc >= 4 && strcmp(verb, "--rename-session") == 0) {
		/* No fallback to argv[2] as a session name: a pid that matches
		   no agent was being handed to tmux as a target, which resolved
		   to an unrelated session and renamed it. Refuse instead. */
		if (found == NULL || found->pane_id[0] == '\0') {
			notify("agent-sidebar: no agent with that pid");
			return 2;
		}
		char old[64], want[256], esc[512], got[256];
		snprintf(old, sizeof old, "%s", found->sess);
		if (strcmp(argv[3], "-") == 0)
			take_input(want, sizeof want);
		else
			snprintf(want, sizeof want, "%s", argv[3]);
		if (want[0] == '\0') {
			notify("agent-sidebar: rename needs a name");
			return 2;
		}
		if (fmt_escape(esc, sizeof esc, want) != 0) {
			notify("agent-sidebar: that name is too long");
			return 2;
		}
		char *rn[] = { (char *)"tmux", (char *)"rename-session",
			       (char *)"-t", found->pane_id, (char *)"--", esc,
			       NULL };
		if (run_tmux(rn) != 0) {
			notify("agent-sidebar: rename failed");
			return 1;
		}
		char *now[] = { (char *)"tmux", (char *)"display-message", (char *)"-p",
				(char *)"-t", found->pane_id,
				(char *)"#{session_name}", NULL };
		if (tmux_capture(now, got, sizeof got) == 0)
			got[strcspn(got, "\n")] = '\0';
		else
			snprintf(got, sizeof got, "%s", want);
		lists_rename(old, got);
		agents_refresh_panes();
		return 0;
	}
	return -1;
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

	if (argc >= 2 && strncmp(argv[1], "--", 2) == 0) {
		int rc = action(argc, argv);
		if (rc >= 0)
			return rc;
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
	read_config();

	int cols, rows;
	term_size(STDOUT_FILENO, &cols, &rows);
	if (force_width > 0)
		cols = force_width;

	if (list) {
		int n = agents_load(agents, AGENT_MAX);
		long long now = now_ms();
		for (int i = 0; i < n; i++) {
			const agent *a = &agents[i];
			char age[16];
			render_age(age, sizeof age, now, a->seen_ms);

			/* hidden: pid, pane. shown: state, age, session, path.
			   last, for actions: the kind */
			printf("%lld\t%s\t%s%-7s\033[0m\t%5s\t%-18s\t%s\t%s\n",
			       a->pid,
			       a->pane_id[0] ? a->pane_id : "-",
			       a->parked ? "\033[38;5;244m"
			       : a->frozen ? frozen_colour()
					   : status_colour_for(a->status),
			       a->parked ? "parked"
			       : a->frozen ? "frozen"
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

	static tsession sessions[64];
	char self[64];

	if (once) {
		int n = agents_load(agents, AGENT_MAX);
		agents_self_session(self, sizeof self);
		int nsess = g_show_sessions
				    ? agents_sessions(sessions, 64, agents, n,
						      g_exclude)
				    : 0;
		render_build(&cur, agents, n, cols,
			     force_rows > 0 ? force_rows : FRAME_ROWS,
			     now_ms(), self, sessions, nsess, g_legend[0] != '\0');
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

	/* the agents' state, the plugins' and ours (lists, the pane map) */
	char dirs[3][512];
	sessions_dir(dirs[0], sizeof dirs[0]);
	agents_state_path(dirs[1], sizeof dirs[1], "agents");
	agents_state_path(dirs[2], sizeof dirs[2], "");
	mkdir_p(dirs[1]); /* so the watch holds before a producer has run */

	int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	int watches = 0, state_wd = -1;
	if (ifd >= 0) {
		const uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO |
				      IN_CREATE | IN_DELETE | IN_MODIFY |
				      IN_ATTRIB;
		for (int i = 0; i < 3; i++) {
			int wd = inotify_add_watch(ifd, dirs[i], mask);
			if (wd >= 0)
				watches++;
			if (i == 2)
				state_wd = wd;
		}
	}
	if (watches == 0 && ifd >= 0) {
		close(ifd);
		ifd = -1; /* fall back to the poll timeout alone */
	}

	trace("start: stdin isatty=%d TMUX_PANE=%s", isatty(STDIN_FILENO),
	      getenv("TMUX_PANE") ? getenv("TMUX_PANE") : "(unset)");
	atexit(tty_restore);
	/* The alternate screen is never reflowed by tmux. On the normal screen a
	   narrower pane wraps every row, and the wrapped tails stayed below the
	   repainted frame as a stale copy of it. */
	emit("\033[?1049h\033[?25l\033[2J");
	if (isatty(STDIN_FILENO))
		tty_setup();

	prev.rows = 0;
	int force = 1, load = 1, redraw = 1, n = 0, nsess = 0;
	long long next_tick = 0, next_load = 0;

	while (!g_stop) {
		if (g_winch) {
			g_winch = 0;
			pane_size(&cols, &rows);
			force = 1;
			redraw = 1;
		}

		/* files on a change, or every 5s for the liveness checks; the
		   1s tick only redraws, to keep the age column honest */
		long long now = mono_ms();
		if (load || now >= next_load) {
			self_reexec_if_replaced();
			n = agents_load(agents, AGENT_MAX);
			agents_self_session(self, sizeof self);
			nsess = g_show_sessions
					? agents_sessions(sessions, 64, agents, n,
							  g_exclude)
					: 0;
			load = 0;
			redraw = 1;
			next_load = now + 5000;
		}
		if (redraw || now >= next_tick) {
			render_build(&cur, agents, n, cols, rows, now_ms(), self,
				     sessions, nsess, g_legend[0] != '\0');
			render_flush(STDOUT_FILENO, &cur, &prev, force);
			force = 0;
			redraw = 0;
			next_tick = now + 1000;
		}

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

		long long wait = (next_tick < next_load ? next_tick : next_load) - mono_ms();
		int rc = poll(nfds > 0 ? pfd : NULL, (nfds_t)nfds,
			      wait > 0 ? (int)wait : 0);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rc > 0 && slot_inotify >= 0 &&
		    (pfd[slot_inotify].revents & POLLIN))
			load |= events_matter(ifd, state_wd);
		if (rc > 0 && slot_stdin >= 0) {
			if (pfd[slot_stdin].revents & (POLLHUP | POLLERR | POLLNVAL))
				break; /* the pane is gone */
			if (pfd[slot_stdin].revents & POLLIN) {
				handle_input(&cur, agents, n);
				redraw = 1;
			}
		}
	}
	return 0;
}

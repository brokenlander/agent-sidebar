#define _POSIX_C_SOURCE 200809L

#include "agents.h"
#include "json.h"

#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FILE_MAX 65536

static int read_file(const char *path, char *buf, size_t cap, size_t *out_len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	size_t got = 0;
	for (;;) {
		if (got >= cap) {
			close(fd);
			return -1; /* larger than we are willing to parse */
		}
		ssize_t r = read(fd, buf + got, cap - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	*out_len = got;
	return 0;
}

/* Field 22 of /proc/<pid>/stat. The comm field can contain spaces and
   parentheses, so counting starts after the LAST ')': the token right after
   it is field 3, which puts starttime 20 tokens further along. */
static long long proc_starttime(long long pid)
{
	char path[64];
	char buf[1024];
	size_t len;

	if (snprintf(path, sizeof path, "/proc/%lld/stat", pid) < 0)
		return -1;
	if (read_file(path, buf, sizeof buf - 1, &len) != 0)
		return -1;
	buf[len] = '\0';

	char *close_paren = strrchr(buf, ')');
	if (close_paren == NULL)
		return -1;

	char *p = close_paren + 1;

	/* The token after ')' is the state. A zombie has exited and is merely
	   waiting to be reaped, so it is not a live agent - without this a
	   killed agent lingers in the list until its parent gets round to it. */
	while (*p == ' ')
		p++;
	if (*p == 'Z')
		return -1;

	for (int field = 0; field < 19; field++) {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			return -1;
		while (*p != ' ' && *p != '\0')
			p++;
	}
	while (*p == ' ')
		p++;
	if (*p == '\0')
		return -1;

	return strtoll(p, NULL, 10);
}


/* The "tmux" field is only written by newer clients - 9 of 16 sessions on this
   machine lack it while sitting in a pane. Falling back to the process's
   controlling tty covers them. tty_nr is field 7 of /proc/<pid>/stat, encoded
   with the minor split across two ranges. */
static int proc_tty(long long pid, char *dst, size_t cap)
{
	char path[64];
	char buf[1024];
	size_t len;

	if (snprintf(path, sizeof path, "/proc/%lld/stat", pid) < 0)
		return -1;
	if (read_file(path, buf, sizeof buf - 1, &len) != 0)
		return -1;
	buf[len] = '\0';

	char *p = strrchr(buf, ')');
	if (p == NULL)
		return -1;
	p++;

	/* after ')' the tokens are state(3) ppid(4) pgrp(5) session(6)
	   tty_nr(7), so skip four and read the fifth */
	for (int t = 0; t < 4; t++) {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			return -1;
		while (*p != ' ' && *p != '\0')
			p++;
	}
	while (*p == ' ')
		p++;
	if (*p == '\0')
		return -1;

	long tty_nr = strtol(p, NULL, 10);
	if (tty_nr <= 0)
		return -1;

	unsigned long major = ((unsigned long)tty_nr >> 8) & 0xfffu;
	unsigned long minor = ((unsigned long)tty_nr & 0xffu) |
			      (((unsigned long)tty_nr >> 12) & 0xfff00u);

	if (major >= 136 && major <= 143)
		snprintf(dst, cap, "/dev/pts/%lu", minor);
	else if (major == 4)
		snprintf(dst, cap, "/dev/tty%lu", minor);
	else
		return -1;
	return 0;
}

#define TTYMAP_MAX 512

struct ttyrow {
	char tty[64];
	char target[64];
	char sess[64];
	char pane_id[32];
};

/* One `tmux list-panes` for the whole fleet, cached, and only ever run when
   some agent is missing its pane. Keeps the steady state off the tmux server. */
/* Reads tmux's output without a shell. popen would spawn /bin/sh purely to
   exec tmux, doubling the process count for no benefit. */
static int tmux_capture(char *const argv[], char *out, size_t cap)
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
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
			close(null);
		}
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

/* One tmux call for the whole fleet, cached, so the steady state stays off
   the server. */
static int tty_map_load(struct ttyrow *rows, int cap)
{
	static char out[65536];
	char *argv[] = {
		(char *)"tmux", (char *)"list-panes", (char *)"-a",
		(char *)"-F",
		(char *)"#{pane_tty}\t#{session_name}\t"
			"#{session_name}:#{window_index}.#{pane_index}\t"
			"#{pane_id}",
		NULL,
	};

	if (tmux_capture(argv, out, sizeof out) != 0)
		return 0;

	int n = 0;
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save);
	     line != NULL && n < cap; line = strtok_r(NULL, "\n", &save)) {
		char *t1 = strchr(line, '\t');
		if (t1 == NULL)
			continue;
		*t1 = '\0';
		char *t2 = strchr(t1 + 1, '\t');
		if (t2 == NULL)
			continue;
		*t2 = '\0';
		char *t3 = strchr(t2 + 1, '\t');
		if (t3 != NULL)
			*t3 = '\0';

		snprintf(rows[n].tty, sizeof rows[n].tty, "%.*s",
			 (int)(sizeof rows[n].tty - 1), line);
		snprintf(rows[n].sess, sizeof rows[n].sess, "%.*s",
			 (int)(sizeof rows[n].sess - 1), t1 + 1);
		snprintf(rows[n].target, sizeof rows[n].target, "%.*s",
			 (int)(sizeof rows[n].target - 1), t2 + 1);
		snprintf(rows[n].pane_id, sizeof rows[n].pane_id, "%.*s",
			 (int)(sizeof rows[n].pane_id - 1),
			 t3 != NULL ? t3 + 1 : "");
		n++;
	}
	return n;
}

/* How long a pane map may be reused. tmux answers list-panes in ~4ms, so a
   short TTL costs nothing; an earlier version cached per pid forever, which
   was right when a tmux call took seconds but left a renamed session showing
   its old name indefinitely. */
#define TTYMAP_TTL 5

static int map_nrows;
static time_t map_fetched;
static struct ttyrow map_rows[TTYMAP_MAX];

void agents_invalidate(void)
{
	map_fetched = 0;
}

static void resolve_panes(agent *a, int n)
{
	struct ttyrow *rows = map_rows;
	int nrows = map_nrows;
	time_t fetched = map_fetched;

	time_t now = time(NULL);
	if (nrows == 0 || fetched == 0 || now - fetched >= TTYMAP_TTL) {
		nrows = tty_map_load(rows, TTYMAP_MAX);
		fetched = now;
	}
	map_nrows = nrows;
	map_fetched = fetched;
	if (nrows == 0)
		return;

	/* Resolve every agent, not only the ones missing a pane: the session
	   name in the JSON is written once at startup and does not follow a
	   rename, so tmux is the authority for it. */
	int unresolved = 0;
	for (int pass = 0; pass < 2; pass++) {
		unresolved = 0;
		for (int i = 0; i < n; i++) {
			char tty[64];
			if (proc_tty(a[i].pid, tty, sizeof tty) != 0)
				continue;

			int found = 0;
			for (int r = 0; r < nrows; r++) {
				if (strcmp(rows[r].tty, tty) != 0)
					continue;
				snprintf(a[i].pane, sizeof a[i].pane, "%s",
					 rows[r].target);
				snprintf(a[i].sess, sizeof a[i].sess, "%s",
					 rows[r].sess);
				snprintf(a[i].pane_id, sizeof a[i].pane_id,
					 "%s", rows[r].pane_id);
				found = 1;
				break;
			}
			if (!found)
				unresolved++;
		}

		/* A pane created since the last load would otherwise wait out
		   the TTL before appearing. */
		if (unresolved == 0 || now - fetched < 1)
			break;
		nrows = tty_map_load(rows, TTYMAP_MAX);
		fetched = now;
		map_nrows = nrows;
		map_fetched = fetched;
	}
}

/* The session this sidebar runs in, read from the same cached pane map so
   it follows a rename like every other row. */
int agents_self_session(char *dst, size_t cap)
{
	const char *pane = getenv("TMUX_PANE");

	dst[0] = '\0';
	if (pane == NULL || *pane == '\0')
		return 0;
	for (int r = 0; r < map_nrows; r++) {
		if (strcmp(map_rows[r].pane_id, pane) == 0) {
			snprintf(dst, cap, "%s", map_rows[r].sess);
			return 1;
		}
	}
	return 0;
}

/* A session name matched by the exclude pattern - a shell-style glob list, the
   alternatives separated by '|', as @agent_sidebar_exclude takes. */
static int name_excluded(const char *sess, const char *pat)
{
	if (pat == NULL || pat[0] == '\0')
		return 0;
	char buf[256];
	snprintf(buf, sizeof buf, "%s", pat);
	char *save = NULL;
	for (char *p = strtok_r(buf, "|", &save); p != NULL;
	     p = strtok_r(NULL, "|", &save))
		if (fnmatch(p, sess, 0) == 0)
			return 1;
	return 0;
}

int agents_sessions(char out[][64], int cap, const agent *a, int n,
		    const char *exclude)
{
	int m = 0;

	for (int r = 0; r < map_nrows && m < cap; r++) {
		const char *sess = map_rows[r].sess;
		if (sess[0] == '\0' || name_excluded(sess, exclude))
			continue;

		int skip = 0;
		for (int i = 0; i < n; i++)      /* a session holding an agent */
			if (strcmp(a[i].sess, sess) == 0) { skip = 1; break; }
		for (int k = 0; !skip && k < m; k++) /* already listed */
			if (strcmp(out[k], sess) == 0) { skip = 1; break; }
		if (skip)
			continue;

		snprintf(out[m], 64, "%s", sess);
		m++;
	}

	for (int i = 1; i < m; i++) {         /* stable order: sort by name */
		char tmp[64];
		memcpy(tmp, out[i], 64);
		int j = i - 1;
		while (j >= 0 && strcmp(out[j], tmp) > 0) {
			memcpy(out[j + 1], out[j], 64);
			j--;
		}
		memcpy(out[j + 1], tmp, 64);
	}
	return m;
}

static agent_status parse_status(const char *s)
{
	if (strcmp(s, "waiting") == 0 || strcmp(s, "blocked") == 0)
		return ST_WAITING;
	/* a backgrounded shell command is not a state you act on differently */
	if (strcmp(s, "idle") == 0 || strcmp(s, "shell") == 0)
		return ST_IDLE;
	if (strcmp(s, "busy") == 0 || strcmp(s, "working") == 0)
		return ST_BUSY;
	return ST_UNKNOWN;
}

const char *agent_status_label(agent_status s)
{
	switch (s) {
	case ST_WAITING: return "waiting";
	case ST_IDLE:    return "idle";
	case ST_BUSY:    return "working";
	default:         return "?";
	}
}

static void collapse_home(char *path, size_t cap)
{
	const char *home = getenv("HOME");
	if (home == NULL || *home == '\0')
		return;

	size_t hn = strlen(home);
	if (strncmp(path, home, hn) != 0)
		return;
	if (path[hn] != '/' && path[hn] != '\0')
		return;

	char tmp[256];
	int w = snprintf(tmp, sizeof tmp, "~%s", path + hn);
	if (w < 0)
		return;
	snprintf(path, cap, "%s", tmp);
}

static void park_path(char *dst, size_t cap)
{
	const char *xdg = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");

	if (xdg != NULL && *xdg != '\0')
		snprintf(dst, cap, "%s/agent-sidebar/parked", xdg);
	else
		snprintf(dst, cap, "%s/.local/state/agent-sidebar/parked",
			 home != NULL ? home : ".");
}

#define PARK_MAX 128

static char park_list[PARK_MAX][64];
static int park_n;

static void park_load(void)
{
	char path[512];
	park_path(path, sizeof path);
	park_n = 0;

	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return;

	char line[128];
	while (park_n < PARK_MAX && fgets(line, sizeof line, fp) != NULL) {
		char *nl = strchr(line, '\n');
		if (nl != NULL)
			*nl = '\0';
		if (line[0] == '\0')
			continue;
		snprintf(park_list[park_n], sizeof park_list[park_n], "%.*s",
			 (int)(sizeof park_list[park_n] - 1), line);
		park_n++;
	}
	fclose(fp);
}

static int park_contains(const char *sess)
{
	if (sess == NULL || *sess == '\0')
		return 0;
	for (int i = 0; i < park_n; i++)
		if (strcmp(park_list[i], sess) == 0)
			return 1;
	return 0;
}

int agents_park_has(const char *sess)
{
	park_load();
	return park_contains(sess);
}

void agents_park_toggle(const char *sess)
{
	char path[512];
	char dir[512];

	if (sess == NULL || *sess == '\0')
		return;

	park_load();

	int at = -1;
	for (int i = 0; i < park_n; i++)
		if (strcmp(park_list[i], sess) == 0)
			at = i;

	if (at >= 0) {
		for (int i = at; i < park_n - 1; i++)
			memcpy(park_list[i], park_list[i + 1],
			       sizeof park_list[i]);
		park_n--;
	} else if (park_n < PARK_MAX) {
		snprintf(park_list[park_n], sizeof park_list[park_n], "%.*s",
			 (int)(sizeof park_list[park_n] - 1), sess);
		park_n++;
	}

	park_path(path, sizeof path);
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
		char mk[600];
		snprintf(mk, sizeof mk, "mkdir -p '%s'", dir);
		int rc = system(mk);
		(void)rc;
	}

	FILE *fp = fopen(path, "w");
	if (fp == NULL)
		return;
	for (int i = 0; i < park_n; i++)
		fprintf(fp, "%s\n", park_list[i]);
	fclose(fp);
}

static int cmp_agent(const void *a, const void *b)
{
	const agent *x = a, *y = b;

	/* muted agents sink below everything, whatever they are doing */
	if (x->parked != y->parked)
		return x->parked - y->parked;
	if (x->status != y->status)
		return (int)x->status - (int)y->status;
	/* freshest first inside a group */
	if (x->seen_ms > y->seen_ms)
		return -1;
	if (x->seen_ms < y->seen_ms)
		return 1;
	return 0;
}

static int scan_dir(const char *dir, agent *out, int cap, int n)
{
	DIR *d = opendir(dir);
	if (d == NULL)
		return n; /* a producer that is not installed is not an error */

	static char buf[FILE_MAX];
	struct dirent *e;

	while ((e = readdir(d)) != NULL && n < cap) {
		size_t nl = strlen(e->d_name);
		if (nl < 6 || strcmp(e->d_name + nl - 5, ".json") != 0)
			continue;

		char path[1024];
		if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) < 0)
			continue;

		size_t len;
		if (read_file(path, buf, sizeof buf, &len) != 0)
			continue;

		jobj o;
		if (json_parse(buf, len, &o) != 0)
			continue;

		char kind[32];
		if (json_string(json_get(&o, "kind"), kind, sizeof kind) != 0)
			continue;
		if (strcmp(kind, "interactive") != 0)
			continue;

		long long pid = json_int(json_get(&o, "pid"), -1);
		if (pid <= 0)
			continue;

		/* procStart guards against PID reuse handing us a stale row */
		long long want = json_int(json_get(&o, "procStart"), -1);
		if (want < 0 || proc_starttime(pid) != want)
			continue;

		agent *a = &out[n];
		memset(a, 0, sizeof *a);
		a->pid = pid;

		char st[32];
		if (json_string(json_get(&o, "status"), st, sizeof st) != 0)
			st[0] = '\0';
		a->status = parse_status(st);

		json_string(json_get(&o, "name"), a->name, sizeof a->name);
		json_string(json_get(&o, "cwd"), a->cwd, sizeof a->cwd);
		json_string(json_get(&o, "agent"), a->agent, sizeof a->agent);
		collapse_home(a->cwd, sizeof a->cwd);

		const jslice *tv = json_get(&o, "tmux");
		if (!json_is_null(tv))
			json_string(tv, a->pane, sizeof a->pane);

		/* session name is everything before the first ':' */
		const char *colon = strchr(a->pane, ':');
		if (colon != NULL) {
			size_t sn = (size_t)(colon - a->pane);
			if (sn >= sizeof a->sess)
				sn = sizeof a->sess - 1;
			memcpy(a->sess, a->pane, sn);
			a->sess[sn] = '\0';
		}

		const char *dot = strrchr(a->pane, '.');
		if (dot != NULL && dot[1] == '%')
			snprintf(a->pane_id, sizeof a->pane_id, "%s", dot + 1);

		a->seen_ms = json_int(json_get(&o, "statusUpdatedAt"),
				      json_int(json_get(&o, "updatedAt"), 0));
		n++;
	}
	closedir(d);
	return n;
}

/* Where each producer writes. Claude Code writes its own natively; everything
   else goes through a plugin into our state dir. Same file shape either way,
   so this is one scanner over two directories. */
int agents_load(agent *out, int cap)
{
	char claude_dir[512];
	char plugin_dir[512];
	const char *home = getenv("HOME");
	const char *cfg = getenv("CLAUDE_CONFIG_DIR");
	const char *xdg = getenv("XDG_STATE_HOME");

	if (cfg != NULL && *cfg != '\0')
		snprintf(claude_dir, sizeof claude_dir, "%s/sessions", cfg);
	else if (home != NULL)
		snprintf(claude_dir, sizeof claude_dir, "%s/.claude/sessions",
			 home);
	else
		claude_dir[0] = '\0';

	if (xdg != NULL && *xdg != '\0')
		snprintf(plugin_dir, sizeof plugin_dir,
			 "%s/agent-sidebar/agents", xdg);
	else if (home != NULL)
		snprintf(plugin_dir, sizeof plugin_dir,
			 "%s/.local/state/agent-sidebar/agents", home);
	else
		plugin_dir[0] = '\0';

	int n = 0;
	if (claude_dir[0] != '\0')
		n = scan_dir(claude_dir, out, cap, n);
	if (plugin_dir[0] != '\0')
		n = scan_dir(plugin_dir, out, cap, n);

	if (n == 0)
		return 0;

	resolve_panes(out, n);

	park_load();
	for (int i = 0; i < n; i++)
		out[i].parked = park_contains(out[i].sess);

	qsort(out, (size_t)n, sizeof *out, cmp_agent);
	return n;
}

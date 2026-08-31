#define _POSIX_C_SOURCE 200809L

#include "agents.h"
#include "json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
static int tty_map_load(struct ttyrow *rows, int cap)
{
	FILE *fp = popen("tmux list-panes -a -F "
			 "'#{pane_tty}\t#{session_name}\t"
			 "#{session_name}:#{window_index}.#{pane_index}\t"
			 "#{pane_id}' "
			 "2>/dev/null", "r");
	if (fp == NULL)
		return 0;

	char line[512];
	int n = 0;
	while (n < cap && fgets(line, sizeof line, fp) != NULL) {
		char *nl = strchr(line, '\n');
		if (nl != NULL)
			*nl = '\0';

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
	pclose(fp);
	return n;
}

static void resolve_panes(agent *a, int n)
{
	static struct ttyrow rows[TTYMAP_MAX];
	/* pid -> pane, resolved once and kept: a live agent does not migrate
	   panes, so this reduces the tmux round trips to one per new agent
	   rather than one every refresh. */
	static struct {
		long long pid;
		char target[64];
		char sess[64];
		char pane_id[32];
	} cache[AGENT_MAX];
	static int ncache;

	int unknown = 0;
	for (int i = 0; i < n; i++) {
		if (a[i].pane[0] != '\0')
			continue;

		int hit = 0;
		for (int c = 0; c < ncache; c++) {
			if (cache[c].pid != a[i].pid)
				continue;
			snprintf(a[i].pane, sizeof a[i].pane, "%s",
				 cache[c].target);
			snprintf(a[i].sess, sizeof a[i].sess, "%s",
				 cache[c].sess);
			snprintf(a[i].pane_id, sizeof a[i].pane_id, "%s",
				 cache[c].pane_id);
			hit = 1; /* an empty entry means "known: not in tmux" */
			break;
		}
		if (!hit)
			unknown++;
	}
	if (unknown == 0)
		return;

	int nrows = tty_map_load(rows, TTYMAP_MAX);
	if (nrows == 0)
		return;

	for (int i = 0; i < n; i++) {
		if (a[i].pane[0] != '\0')
			continue;

		char tty[64];
		int found = -1;
		if (proc_tty(a[i].pid, tty, sizeof tty) == 0) {
			for (int r = 0; r < nrows; r++) {
				if (strcmp(rows[r].tty, tty) == 0) {
					found = r;
					break;
				}
			}
		}

		if (found >= 0) {
			snprintf(a[i].pane, sizeof a[i].pane, "%s",
				 rows[found].target);
			snprintf(a[i].sess, sizeof a[i].sess, "%s",
				 rows[found].sess);
			snprintf(a[i].pane_id, sizeof a[i].pane_id, "%s",
				 rows[found].pane_id);
		}

		/* record the outcome either way, so an agent that is not in
		   tmux is asked about once rather than on every refresh */
		if (ncache < AGENT_MAX) {
			cache[ncache].pid = a[i].pid;
			snprintf(cache[ncache].target,
				 sizeof cache[ncache].target, "%s",
				 found >= 0 ? rows[found].target : "");
			snprintf(cache[ncache].sess,
				 sizeof cache[ncache].sess, "%s",
				 found >= 0 ? rows[found].sess : "");
			snprintf(cache[ncache].pane_id,
				 sizeof cache[ncache].pane_id, "%s",
				 found >= 0 ? rows[found].pane_id : "");
			ncache++;
		}
	}
}

static agent_status parse_status(const char *s)
{
	if (strcmp(s, "waiting") == 0 || strcmp(s, "blocked") == 0)
		return ST_WAITING;
	if (strcmp(s, "idle") == 0)
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
		snprintf(dst, cap, "%s/claude-sidebar/parked", xdg);
	else
		snprintf(dst, cap, "%s/.local/state/claude-sidebar/parked",
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

int agents_load(agent *out, int cap)
{
	char dir[512];
	const char *cfg = getenv("CLAUDE_CONFIG_DIR");

	if (cfg != NULL && *cfg != '\0') {
		snprintf(dir, sizeof dir, "%s/sessions", cfg);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL)
			return -1;
		snprintf(dir, sizeof dir, "%s/.claude/sessions", home);
	}

	DIR *d = opendir(dir);
	if (d == NULL)
		return -1;

	static char buf[FILE_MAX];
	int n = 0;
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

	resolve_panes(out, n);

	park_load();
	for (int i = 0; i < n; i++)
		out[i].parked = park_contains(out[i].sess);

	qsort(out, (size_t)n, sizeof *out, cmp_agent);
	return n;
}

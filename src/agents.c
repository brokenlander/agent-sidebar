#define _POSIX_C_SOURCE 200809L

#include "agents.h"
#include "json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

/* Fields 22 (starttime) and 7 (tty), counted after the LAST ')' since comm
   may hold both; a zombie reads as gone. */
static int proc_stat(long long pid, long long *start, char *tty, size_t cap)
{
	char path[64];
	char buf[1024];
	size_t len;

	tty[0] = '\0';
	snprintf(path, sizeof path, "/proc/%lld/stat", pid);
	if (read_file(path, buf, sizeof buf - 1, &len) != 0)
		return -1;
	buf[len] = '\0';

	char *p = strrchr(buf, ')');
	if (p == NULL)
		return -1;
	p++;

	long long field[20];
	for (int f = 0; f < 20; f++) {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			return -1;
		if (f == 0 && *p == 'Z')
			return -1;
		field[f] = strtoll(p, NULL, 10);
		while (*p != ' ' && *p != '\0')
			p++;
	}
	*start = field[19];

	unsigned long nr = (unsigned long)field[4];
	unsigned long major = (nr >> 8) & 0xfffu;
	unsigned long minor = (nr & 0xffu) | ((nr >> 12) & 0xfff00u);
	if (nr == 0)
		return 0;
	if (major >= 136 && major <= 143)
		snprintf(tty, cap, "/dev/pts/%lu", minor);
	else if (major == 4)
		snprintf(tty, cap, "/dev/tty%lu", minor);
	return 0;
}

long long now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long wall_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return 0;
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long mtime_ns(const struct stat *st)
{
	return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
}

void agents_state_path(char *dst, size_t cap, const char *leaf)
{
	const char *xdg = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");

	if (xdg != NULL && *xdg != '\0')
		snprintf(dst, cap, "%s/agent-sidebar/%s", xdg, leaf);
	else
		snprintf(dst, cap, "%s/.local/state/agent-sidebar/%s",
			 home != NULL ? home : ".", leaf);
}

void mkdir_p(const char *path)
{
	char buf[512];

	snprintf(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		mkdir(buf, 0755);
		*p = '/';
	}
	mkdir(buf, 0755);
}

/* stderr goes nowhere: inherited, tmux's errors print into the sidebar's pane. */
int tmux_capture(char *const argv[], char *out, size_t cap)
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

/* One pane map file per tmux server, shared by every sidebar: see "Why it is
   cheap" in the README. */
#define TTYMAP_MAX 512
#define MAP_TTL_NS (10LL * 1000000000LL)

struct ttyrow {
	char tty[32];
	char sess_id[16];
	char pane_id[16];
	char sess[64];
};

static struct ttyrow map_rows[TTYMAP_MAX];
static int map_nrows;
static long long map_ver; /* when the rows were fetched */
static long long map_ttl; /* staggered per process, so one refreshes and the rest read */
static char map_file[512];

const char *agents_map_path(void)
{
	const char *t = getenv("TMUX");
	char leaf[160] = "panes";

	if (map_file[0] != '\0')
		return map_file;
	map_ttl = MAP_TTL_NS + (long long)(getpid() % 2000) * 1000000LL;
	if (t != NULL && *t != '\0') {
		size_t n = strcspn(t, ",");
		size_t b = n;
		while (b > 0 && t[b - 1] != '/')
			b--;
		snprintf(leaf, sizeof leaf, "panes-%.*s", (int)(n - b), t + b);
	}
	agents_state_path(map_file, sizeof map_file, leaf);
	return map_file;
}

/* the name goes last: it is the one field that could hold a tab */
static void map_parse(char *out)
{
	int n = 0;
	char *save = NULL;

	for (char *line = strtok_r(out, "\n", &save);
	     line != NULL && n < TTYMAP_MAX; line = strtok_r(NULL, "\n", &save)) {
		char *f[4] = { line, NULL, NULL, NULL };
		for (int i = 1; i < 4; i++) {
			f[i] = strchr(f[i - 1], '\t');
			if (f[i] == NULL)
				break;
			*f[i]++ = '\0';
		}
		if (f[3] == NULL)
			continue;
		snprintf(map_rows[n].tty, sizeof map_rows[n].tty, "%s", f[0]);
		snprintf(map_rows[n].sess_id, sizeof map_rows[n].sess_id, "%s", f[1]);
		snprintf(map_rows[n].pane_id, sizeof map_rows[n].pane_id, "%s", f[2]);
		snprintf(map_rows[n].sess, sizeof map_rows[n].sess, "%s", f[3]);
		n++;
	}
	map_nrows = n;
}

static char map_buf[65536];

static void map_read(void)
{
	int fd = open(map_file, O_RDONLY | O_CLOEXEC);
	struct stat st;
	size_t got = 0;

	if (fd < 0)
		return;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return;
	}
	while (got < sizeof map_buf - 1) {
		ssize_t r = read(fd, map_buf + got, sizeof map_buf - 1 - got);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	map_buf[got] = '\0';
	map_parse(map_buf);
	map_ver = mtime_ns(&st);
}

static void map_fetch(void)
{
	char *argv[] = {
		(char *)"tmux", (char *)"list-panes", (char *)"-a", (char *)"-F",
		(char *)"#{pane_tty}\t#{session_id}\t#{pane_id}\t#{session_name}",
		NULL,
	};
	char tmp[600];
	struct stat st;

	if (tmux_capture(argv, map_buf, sizeof map_buf) != 0)
		return;
	map_ver = wall_ns();

	snprintf(tmp, sizeof tmp, "%s.%ld", map_file, (long)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		size_t len = strlen(map_buf);
		int ok = write(fd, map_buf, len) == (ssize_t)len;
		close(fd);
		if (ok && rename(tmp, map_file) == 0 && stat(map_file, &st) == 0)
			map_ver = mtime_ns(&st);
		else
			unlink(tmp);
	}
	map_parse(map_buf);
}

/* need_after: the oldest fetch that will do; 0 takes any within the TTL. */
static void map_sync(long long need_after)
{
	struct stat st;
	long long file = 0;

	agents_map_path();
	if (stat(map_file, &st) == 0)
		file = mtime_ns(&st);
	long long best = file > map_ver ? file : map_ver;
	if (best >= need_after && wall_ns() - best < map_ttl) {
		if (file > map_ver)
			map_read();
		return;
	}

	char dir[512], lock[600];
	snprintf(dir, sizeof dir, "%s", map_file);
	char *slash = strrchr(dir, '/');
	if (slash != NULL)
		*slash = '\0';
	mkdir_p(dir);
	snprintf(lock, sizeof lock, "%s.lock", map_file);
	int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd >= 0) {
		struct flock fl;
		memset(&fl, 0, sizeof fl);
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		while (fcntl(fd, F_SETLKW, &fl) != 0 && errno == EINTR)
			;
	}

	file = stat(map_file, &st) == 0 ? mtime_ns(&st) : 0;
	if (file >= need_after && wall_ns() - file < map_ttl)
		map_read();
	else
		map_fetch();

	if (fd >= 0)
		close(fd);
}

void agents_refresh_panes(void)
{
	map_sync(wall_ns());
}

static const struct ttyrow *map_find_tty(const char *tty)
{
	for (int r = 0; r < map_nrows; r++)
		if (strcmp(map_rows[r].tty, tty) == 0)
			return &map_rows[r];
	return NULL;
}

/* The session this sidebar runs in, from the map so it follows a rename. */
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

int agents_sessions(tsession *out, int cap, const agent *a, int n,
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
			if (strcmp(out[k].name, sess) == 0) { skip = 1; break; }
		if (skip)
			continue;

		snprintf(out[m].name, sizeof out[m].name, "%s", sess);
		snprintf(out[m].id, sizeof out[m].id, "%s", map_rows[r].sess_id);
		m++;
	}

	for (int i = 1; i < m; i++) {         /* stable order: sort by name */
		tsession tmp = out[i];
		int j = i - 1;
		while (j >= 0 && strcmp(out[j].name, tmp.name) > 0) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = tmp;
	}
	return m;
}

static agent_status parse_status(const char *s)
{
	/* Reported by the agent itself: it is blocked on input NOW. Distinct
	   from the idle promotion below, which only means forgotten. */
	if (strcmp(s, "waiting") == 0 || strcmp(s, "blocked") == 0)
		return ST_ASKING;
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
	case ST_ASKING:  return "asking";
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

/* Two lists of session names, one per line, in the state directory: parked
   (muted) and frozen (kept for later). Same file shape, same code. */
#define LIST_MAX 128

typedef struct {
	const char *file;
	char item[LIST_MAX][64];
	int n;
} namelist;

static namelist g_parked = { "parked", { { 0 } }, 0 };
static namelist g_frozen = { "frozen", { { 0 } }, 0 };

static void list_path(const namelist *l, char *dst, size_t cap)
{
	agents_state_path(dst, cap, l->file);
}

static void list_load(namelist *l)
{
	char path[512];
	list_path(l, path, sizeof path);
	l->n = 0;

	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return;

	char line[128];
	while (l->n < LIST_MAX && fgets(line, sizeof line, fp) != NULL) {
		char *nl = strchr(line, '\n');
		if (nl != NULL)
			*nl = '\0';
		if (line[0] == '\0')
			continue;
		snprintf(l->item[l->n], sizeof l->item[l->n], "%.*s",
			 (int)(sizeof l->item[l->n] - 1), line);
		l->n++;
	}
	fclose(fp);
}

static int list_contains(const namelist *l, const char *sess)
{
	if (sess == NULL || *sess == '\0')
		return 0;
	for (int i = 0; i < l->n; i++)
		if (strcmp(l->item[i], sess) == 0)
			return 1;
	return 0;
}

static void list_toggle(namelist *l, const char *sess)
{
	char path[512];
	char dir[512];

	if (sess == NULL || *sess == '\0')
		return;

	list_load(l);

	int at = -1;
	for (int i = 0; i < l->n; i++)
		if (strcmp(l->item[i], sess) == 0)
			at = i;

	if (at >= 0) {
		for (int i = at; i < l->n - 1; i++)
			memcpy(l->item[i], l->item[i + 1], sizeof l->item[i]);
		l->n--;
	} else if (l->n < LIST_MAX) {
		snprintf(l->item[l->n], sizeof l->item[l->n], "%.*s",
			 (int)(sizeof l->item[l->n] - 1), sess);
		l->n++;
	}

	list_path(l, path, sizeof path);
	agents_state_path(dir, sizeof dir, "");
	mkdir_p(dir);

	FILE *fp = fopen(path, "w");
	if (fp == NULL)
		return;
	for (int i = 0; i < l->n; i++)
		fprintf(fp, "%s\n", l->item[i]);
	fclose(fp);
}

int agents_park_has(const char *sess)
{
	list_load(&g_parked);
	return list_contains(&g_parked, sess);
}

void agents_park_toggle(const char *sess)
{
	list_toggle(&g_parked, sess);
}

int agents_freeze_has(const char *sess)
{
	list_load(&g_frozen);
	return list_contains(&g_frozen, sess);
}

void agents_freeze_toggle(const char *sess)
{
	list_toggle(&g_frozen, sess);
}

static int cmp_agent(const void *a, const void *b)
{
	const agent *x = a, *y = b;

	/* muted agents sink below everything, whatever they are doing */
	if (x->parked != y->parked)
		return x->parked - y->parked;
	/* set-aside ones sit just above them, in their own band */
	if (x->frozen != y->frozen)
		return x->frozen - y->frozen;
	/* then the latest change first, whatever colour it is */
	if (x->seen_ms > y->seen_ms)
		return -1;
	if (x->seen_ms < y->seen_ms)
		return 1;
	return 0;
}

static long long g_idle_wait_ms = 30 * 60 * 1000; /* 30 min */

void agents_set_idle_wait(long long secs)
{
	g_idle_wait_ms = secs > 0 ? secs * 1000 : 0;
}

/* Wall-clock ns at which a process started, from its ticks since boot. */
static long long started_ns(long long ticks)
{
	static long long btime = -1;
	static long long hz;

	if (btime < 0) {
		char buf[4096];
		size_t len;
		btime = 0;
		hz = sysconf(_SC_CLK_TCK);
		if (read_file("/proc/stat", buf, sizeof buf - 1, &len) == 0) {
			buf[len] = '\0';
			char *p = strstr(buf, "\nbtime ");
			if (p != NULL)
				btime = strtoll(p + 7, NULL, 10);
		}
	}
	if (hz <= 0)
		return 0;
	return btime * 1000000000LL + ticks * (1000000000LL / hz);
}

/* Each session file, parsed once and kept until its inode, size or mtime
   moves; a crashed agent leaves its file, so live ones are re-checked. */
#define CACHE_MAX 1024
#define LIVE_CHECK 5

typedef struct {
	int used, seen, live;
	int dir;
	char file[256];
	ino_t ino;
	off_t size;
	long long mtime;
	long long proc_start;  /* procStart, as the file and /proc give it */
	long long start_ns;    /* when the process started, wall clock */
	time_t checked;        /* last liveness check */
	char tty[32];
	agent a;
} entry;

static entry cache[CACHE_MAX];
static int cache_top; /* entries past this have never been used */

static entry *cache_get(int dir, const char *file)
{
	entry *spare = NULL;

	for (int i = 0; i < cache_top; i++) {
		if (!cache[i].used) {
			if (spare == NULL)
				spare = &cache[i];
			continue;
		}
		if (cache[i].dir == dir && strcmp(cache[i].file, file) == 0)
			return &cache[i];
	}
	if (spare == NULL && cache_top < CACHE_MAX)
		spare = &cache[cache_top++];
	if (spare != NULL) {
		memset(spare, 0, sizeof *spare);
		spare->used = 1;
		spare->dir = dir;
		snprintf(spare->file, sizeof spare->file, "%s", file);
	}
	return spare;
}

static void parse_entry(entry *c, const char *path)
{
	static char buf[FILE_MAX];
	size_t len;
	jobj o;
	char kind[32], st[32], tmuxf[64];

	c->live = 0;
	if (read_file(path, buf, sizeof buf, &len) != 0 ||
	    json_parse(buf, len, &o) != 0)
		return;
	if (json_string(json_get(&o, "kind"), kind, sizeof kind) != 0 ||
	    strcmp(kind, "interactive") != 0)
		return;

	long long pid = json_int(json_get(&o, "pid"), -1);
	long long want = json_int(json_get(&o, "procStart"), -1);
	long long start;
	/* procStart guards against PID reuse handing us a stale row */
	if (pid <= 0 || want < 0 ||
	    proc_stat(pid, &start, c->tty, sizeof c->tty) != 0 || start != want)
		return;
	c->proc_start = want;
	c->start_ns = started_ns(want);
	c->checked = time(NULL);

	agent *a = &c->a;
	memset(a, 0, sizeof *a);
	a->pid = pid;
	if (json_string(json_get(&o, "status"), st, sizeof st) != 0)
		st[0] = '\0';
	a->status = parse_status(st);
	json_string(json_get(&o, "name"), a->name, sizeof a->name);
	json_string(json_get(&o, "cwd"), a->cwd, sizeof a->cwd);
	json_string(json_get(&o, "agent"), a->agent, sizeof a->agent);
	collapse_home(a->cwd, sizeof a->cwd);

	/* "sess:@win.%pane"; the pane map overrides it, as it follows renames */
	tmuxf[0] = '\0';
	const jslice *tv = json_get(&o, "tmux");
	if (!json_is_null(tv))
		json_string(tv, tmuxf, sizeof tmuxf);
	size_t sn = strcspn(tmuxf, ":");
	if (tmuxf[sn] == ':')
		snprintf(a->sess, sizeof a->sess, "%.*s", (int)sn, tmuxf);
	const char *dot = strrchr(tmuxf, '.');
	if (dot != NULL && dot[1] == '%')
		snprintf(a->pane_id, sizeof a->pane_id, "%s", dot + 1);

	a->seen_ms = json_int(json_get(&o, "statusUpdatedAt"),
			      json_int(json_get(&o, "updatedAt"), 0));
	c->live = 1;
}

static void scan_dir(const char *dir, int which)
{
	DIR *d = opendir(dir);
	if (d == NULL)
		return; /* a producer that is not installed is not an error */

	time_t now = time(NULL);
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		size_t nl = strlen(e->d_name);
		if (nl < 6 || strcmp(e->d_name + nl - 5, ".json") != 0)
			continue;

		struct stat st;
		if (fstatat(dirfd(d), e->d_name, &st, 0) != 0)
			continue;
		entry *c = cache_get(which, e->d_name);
		if (c == NULL)
			continue;
		c->seen = 1;

		if (c->ino == st.st_ino && c->size == st.st_size &&
		    c->mtime == mtime_ns(&st)) {
			if (c->live && now - c->checked >= LIVE_CHECK) {
				long long start;
				char tty[32];
				c->checked = now;
				if (proc_stat(c->a.pid, &start, tty, sizeof tty) != 0 ||
				    start != c->proc_start)
					c->live = 0;
			}
			continue;
		}

		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		c->ino = st.st_ino;
		c->size = st.st_size;
		c->mtime = mtime_ns(&st);
		parse_entry(c, path);
	}
	closedir(d);
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

	if (cfg != NULL && *cfg != '\0')
		snprintf(claude_dir, sizeof claude_dir, "%s/sessions", cfg);
	else
		snprintf(claude_dir, sizeof claude_dir, "%s/.claude/sessions",
			 home != NULL ? home : ".");
	agents_state_path(plugin_dir, sizeof plugin_dir, "agents");

	for (int i = 0; i < cache_top; i++)
		cache[i].seen = 0;
	scan_dir(claude_dir, 0);
	scan_dir(plugin_dir, 1);

	/* Only a map older than an agent can lack its pane; boot time is whole
	   seconds, hence the margin. */
	map_sync(0);
	long long need = 0;
	for (int i = 0; i < cache_top; i++) {
		entry *c = &cache[i];
		if (c->used && !c->seen)
			c->used = 0;
		if (c->used && c->live && c->tty[0] != '\0' &&
		    map_find_tty(c->tty) == NULL &&
		    c->start_ns + 1000000000LL > need)
			need = c->start_ns + 1000000000LL;
	}
	if (need > map_ver)
		map_sync(need);

	int n = 0;
	for (int i = 0; i < cache_top && n < cap; i++) {
		const entry *c = &cache[i];
		if (!c->used || !c->live)
			continue;
		out[n] = c->a;
		const struct ttyrow *row =
			c->tty[0] != '\0' ? map_find_tty(c->tty) : NULL;
		if (row != NULL) {
			snprintf(out[n].sess, sizeof out[n].sess, "%s", row->sess);
			snprintf(out[n].pane_id, sizeof out[n].pane_id, "%s",
				 row->pane_id);
		}
		n++;
	}

	if (g_idle_wait_ms > 0) {
		long long now = now_ms();
		for (int i = 0; i < n; i++) {
			if (out[i].status != ST_IDLE || out[i].seen_ms <= 0)
				continue;
			if (now - out[i].seen_ms >= g_idle_wait_ms)
				out[i].status = ST_WAITING;
		}
	}

	list_load(&g_parked);
	list_load(&g_frozen);
	for (int i = 0; i < n; i++) {
		out[i].parked = list_contains(&g_parked, out[i].sess);
		out[i].frozen = list_contains(&g_frozen, out[i].sess);
	}

	qsort(out, (size_t)n, sizeof *out, cmp_agent);
	return n;
}

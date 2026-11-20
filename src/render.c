#include "render.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A line under construction. Escape sequences go in via lb_raw so they never
   count toward the visible width. */
typedef struct {
	char buf[LINE_MAX];
	size_t len;
	int width;
	int cols;
} linebuf;

static void lb_init(linebuf *lb, int cols)
{
	lb->len = 0;
	lb->width = 0;
	lb->cols = cols;
	lb->buf[0] = '\0';
}

static void lb_raw(linebuf *lb, const char *s)
{
	size_t n = strlen(s);
	if (lb->len + n + 1 >= sizeof lb->buf)
		return;
	memcpy(lb->buf + lb->len, s, n);
	lb->len += n;
	lb->buf[lb->len] = '\0';
}

/* Visible text, truncated to the remaining width. Never splits a UTF-8
   sequence: continuation bytes ride along with their lead byte for free. */
static void lb_text(linebuf *lb, const char *s)
{
	for (size_t i = 0; s[i] != '\0';) {
		unsigned char c = (unsigned char)s[i];
		size_t clen = 1;
		if (c >= 0xF0)
			clen = 4;
		else if (c >= 0xE0)
			clen = 3;
		else if (c >= 0xC0)
			clen = 2;

		for (size_t k = 1; k < clen; k++)
			if (s[i + k] == '\0') {
				clen = k;
				break;
			}

		if (lb->width + 1 > lb->cols)
			return;
		if (lb->len + clen + 1 >= sizeof lb->buf)
			return;

		memcpy(lb->buf + lb->len, s + i, clen);
		lb->len += clen;
		lb->buf[lb->len] = '\0';
		lb->width++;
		i += clen;
	}
}

static void lb_pad_to(linebuf *lb, int col)
{
	while (lb->width < col && lb->width < lb->cols) {
		if (lb->len + 2 >= sizeof lb->buf)
			return;
		lb->buf[lb->len++] = ' ';
		lb->buf[lb->len] = '\0';
		lb->width++;
	}
}

#define C_RESET "\033[0m"
/* ANSI palette colours, not fixed RGB: the sidebar follows whatever theme the
 * terminal has (Omarchy's current theme, Tokyo Night, ...). */
#define C_DIM "\033[90m"    /* bright black: secondary text */
#define C_HEAD "\033[34m"   /* blue: the theme's accent */
#define C_NAME "\033[94m"   /* bright blue: running agents */
#define C_SELF "\033[97m"   /* bright white: the agent of this session */
#define C_FROZEN "\033[36m" /* cyan - kept for later */

static const char *status_colour(agent_status s);

const char *status_colour_for(agent_status s)
{
	return status_colour(s);
}

const char *frozen_colour(void)
{
	return C_FROZEN;
}

static const char *status_colour(agent_status s)
{
	switch (s) {
	case ST_ASKING:  return "\033[35m"; /* magenta - asking you */
	case ST_WAITING: return "\033[33m"; /* yellow - forgotten */
	case ST_IDLE:    return "\033[32m"; /* green - your turn */
	case ST_BUSY:    return "\033[31m"; /* red   - working */
	default:         return C_DIM;
	}
}

void render_age(char *dst, size_t cap, long long now_ms, long long seen_ms)
{
	if (seen_ms <= 0) {
		snprintf(dst, cap, "-");
		return;
	}

	long long s = (now_ms - seen_ms) / 1000;
	if (s < 0)
		s = 0;

	if (s < 60)
		snprintf(dst, cap, "%llds", s);
	else if (s < 3600)
		snprintf(dst, cap, "%lldm", s / 60);
	else if (s < 86400)
		snprintf(dst, cap, "%lldh", s / 3600);
	else
		snprintf(dst, cap, "%lldd", s / 86400);
}

static const char *basename_of(const char *p)
{
	const char *slash = strrchr(p, '/');
	return (slash != NULL && slash[1] != '\0') ? slash + 1 : p;
}

static void put_line(frame *f, const linebuf *lb, int agent_index)
{
	if (f->rows >= FRAME_ROWS)
		return;
	memcpy(f->line[f->rows], lb->buf, lb->len + 1);
	f->row_agent[f->rows] = agent_index;
	f->row_session[f->rows][0] = '\0';
	f->row_sid[f->rows][0] = '\0';
	f->rows++;
}

/* A dim horizontal rule between sections. */
static void section_rule(frame *f, int cols)
{
	linebuf lb;
	lb_init(&lb, cols);
	lb_raw(&lb, C_DIM);
	for (int i = 0; i < cols; i++)
		lb_text(&lb, "\xe2\x94\x80"); /* \u2500 */
	lb_raw(&lb, C_RESET);
	put_line(f, &lb, -1);
}

/* A dim section sub-heading. */
static void section_head(frame *f, int cols, const char *label)
{
	linebuf lb;
	lb_init(&lb, cols);
	lb_raw(&lb, C_DIM);
	lb_text(&lb, label);
	lb_raw(&lb, C_RESET);
	put_line(f, &lb, -1);
}

void render_build(frame *f, const agent *a, int n, int cols, int rows,
		  long long now_ms, const char *self_sess,
		  const tsession *sessions, int nsessions, int keys)
{
	linebuf lb;
	int count[ST_UNKNOWN + 1] = { 0 };

	f->rows = 0;
	f->keys_row = -1;
	if (cols < 12)
		cols = 12;

	int nparked = 0;
	int nfrozen = 0;
	for (int i = 0; i < n; i++) {
		if (a[i].parked)
			nparked++;
		else if (a[i].frozen)
			nfrozen++;
		else
			count[a[i].status]++;
	}

	/* header: total, then a tally that only shows non-empty states */
	lb_init(&lb, cols);
	lb_raw(&lb, C_HEAD);
	lb_text(&lb, " agents");
	lb_raw(&lb, C_RESET C_DIM);
	{
		char t[32];
		snprintf(t, sizeof t, "%d", n);
		lb_pad_to(&lb, cols - (int)strlen(t) - 1);
		lb_text(&lb, t);
	}
	lb_raw(&lb, C_RESET);
	put_line(f, &lb, -1);

	lb_init(&lb, cols);
	if (nparked > 0) {
		char t[32];
		snprintf(t, sizeof t, " %d", nparked);
		lb_raw(&lb, C_DIM);
		lb_text(&lb, " \xe2\x97\x8b");
		lb_text(&lb, t);
		lb_raw(&lb, C_RESET);
	}
	if (nfrozen > 0) {
		char t[32];
		snprintf(t, sizeof t, " %d", nfrozen);
		lb_raw(&lb, C_FROZEN);
		lb_text(&lb, " \xe2\x97\x8f");
		lb_raw(&lb, C_RESET C_DIM);
		lb_text(&lb, t);
		lb_raw(&lb, C_RESET);
	}
	for (int s = 0; s <= ST_UNKNOWN; s++) {
		if (count[s] == 0)
			continue;
		char t[32];
		snprintf(t, sizeof t, " %d", count[s]);
		lb_raw(&lb, status_colour((agent_status)s));
		lb_text(&lb, " \xe2\x97\x8f"); /* ● */
		lb_raw(&lb, C_RESET C_DIM);
		lb_text(&lb, t);
		lb_raw(&lb, C_RESET);
	}
	put_line(f, &lb, -1);

	section_rule(f, cols);

	/* Reserve the bottom so a long agent list never pushes it off: agents
	   fill the top and overflow with a "+N more" line, then the
	   other-sessions section, then the shortcuts button with a blank row
	   under it. */
	int avail = rows - f->rows;
	if (avail < 1)
		avail = 1;

	int keys_h = !keys ? 0 : avail > 2 ? 2 : avail > 1 ? 1 : 0;

	int sess_shown = nsessions;
	int sess_h = nsessions > 0 ? 2 + nsessions : 0;
	if (sess_h > avail - keys_h - 1) { /* trim sessions to fit */
		int budget = avail - keys_h - 1; /* leave >= 1 row for agents */
		if (budget >= 3) {
			sess_shown = budget - 3; /* rule + head + "+N more" */
			sess_h = 2 + sess_shown + 1;
		} else {
			sess_shown = 0;
			sess_h = 0;
		}
	}

	/* One row per agent. When they do not fit, the last line says how many
	   are hidden: silently dropping them would look like they had been
	   deleted, and the ones that vanish are the bottom of the sort, which
	   is exactly where parked agents live. */
	int room = avail - keys_h - sess_h;
	int shown = n;
	if (room < 1)
		room = 1;
	if (n > room)
		shown = room - 1; /* keep a line for the count */
	if (shown < 0)
		shown = 0;

	for (int i = 0; i < shown; i++) {
		const agent *g = &a[i];
		char age[16];
		render_age(age, sizeof age, now_ms, g->seen_ms);

		const char *label = g->sess[0] != '\0' ? g->sess : g->name;
		if (label[0] == '\0')
			label = "?";

		lb_init(&lb, cols);
		if (g->parked) {
			/* a hollow, dimmed dot: still visible, clearly set aside */
			lb_raw(&lb, C_DIM);
			lb_text(&lb, " \xe2\x97\x8b ");
			lb_text(&lb, label);
			lb_raw(&lb, C_RESET);
		} else if (g->frozen) {
			/* deep blue dot and name: kept, not muted */
			lb_raw(&lb, C_FROZEN);
			lb_text(&lb, " \xe2\x97\x8f ");
			lb_text(&lb, label);
			lb_raw(&lb, C_RESET);
		} else {
			int self = self_sess != NULL && self_sess[0] != '\0' &&
				   strcmp(g->sess, self_sess) == 0;
			lb_raw(&lb, status_colour(g->status));
			lb_text(&lb, " \xe2\x97\x8f ");
			lb_raw(&lb, self ? C_RESET C_SELF : C_RESET C_NAME);
			lb_text(&lb, label);
			lb_raw(&lb, C_RESET);
		}

		/* room for the cwd basename only on a wide enough pane */
		int agew = (int)strlen(age);
		if (cols >= 40 && g->cwd[0] != '\0') {
			const char *base = basename_of(g->cwd);
			if (lb.width + 1 + (int)strlen(base) + 1 + agew < cols) {
				lb_pad_to(&lb, lb.width + 1);
				lb_raw(&lb, C_DIM);
				lb_text(&lb, base);
				lb_raw(&lb, C_RESET);
			}
		}

		lb_raw(&lb, C_DIM);
		lb_pad_to(&lb, cols - agew - 1);
		lb_text(&lb, age);
		lb_raw(&lb, C_RESET);
		put_line(f, &lb, i);
	}

	if (shown < n) {
		char t[32];
		snprintf(t, sizeof t, " +%d more", n - shown);
		lb_init(&lb, cols);
		lb_raw(&lb, C_DIM);
		lb_text(&lb, t);
		lb_raw(&lb, C_RESET);
		put_line(f, &lb, -1);
	}

	if (n == 0) {
		lb_init(&lb, cols);
		lb_raw(&lb, C_DIM);
		lb_text(&lb, " no agents");
		lb_raw(&lb, C_RESET);
		put_line(f, &lb, -1);
	}

	/* Other tmux sessions - the ones with no agent. Clickable to jump. */
	if (sess_shown > 0) {
		section_rule(f, cols);
		section_head(f, cols, " sessions");
		for (int i = 0; i < sess_shown; i++) {
			lb_init(&lb, cols);
			lb_raw(&lb, C_DIM);
			lb_text(&lb, " \xc2\xb7 "); /* · */
			lb_raw(&lb, C_RESET C_NAME);
			lb_text(&lb, sessions[i].name);
			lb_raw(&lb, C_RESET);
			put_line(f, &lb, -1);
			snprintf(f->row_session[f->rows - 1],
				 sizeof f->row_session[0], "%s", sessions[i].name);
			snprintf(f->row_sid[f->rows - 1], sizeof f->row_sid[0],
				 "%s", sessions[i].id);
		}
		if (sess_shown < nsessions) {
			char t[32];
			snprintf(t, sizeof t, " +%d more", nsessions - sess_shown);
			lb_init(&lb, cols);
			lb_raw(&lb, C_DIM);
			lb_text(&lb, t);
			lb_raw(&lb, C_RESET);
			put_line(f, &lb, -1);
		}
	}

	/* pinned to the bottom, so it stays put while the list changes */
	if (keys_h > 0) {
		while (rows < FRAME_ROWS && f->rows < rows - keys_h) {
			lb_init(&lb, cols);
			put_line(f, &lb, -1);
		}
		if (f->rows < FRAME_ROWS) {
			lb_init(&lb, cols);
			lb_raw(&lb, C_NAME);
			lb_text(&lb, " ?");
			lb_raw(&lb, C_RESET C_DIM);
			lb_text(&lb, " shortcuts");
			lb_raw(&lb, C_RESET);
			f->keys_row = f->rows;
			put_line(f, &lb, -1);
		}
		if (keys_h > 1 && rows < FRAME_ROWS) {
			lb_init(&lb, cols);
			put_line(f, &lb, -1);
		}
	}
}

/* Only the rows in use: the whole frame is a quarter of a megabyte. */
static void render_copy(frame *dst, const frame *src)
{
	for (int i = 0; i < src->rows; i++) {
		memcpy(dst->line[i], src->line[i], strlen(src->line[i]) + 1);
		dst->row_agent[i] = src->row_agent[i];
		memcpy(dst->row_session[i], src->row_session[i], sizeof dst->row_session[i]);
		memcpy(dst->row_sid[i], src->row_sid[i], sizeof dst->row_sid[i]);
	}
	dst->rows = src->rows;
	dst->keys_row = src->keys_row;
}

long render_flush(int fd, const frame *cur, frame *prev, int force)
{
	char out[8192];
	size_t o = 0;
	long total = 0;

	/* A forced repaint follows a resize, and the frame's rows are all it
	   knows about. Clear the screen and its history so nothing tmux moved
	   or wrapped outside those rows survives. Never address a row past a
	   shrunken pane: the cursor clamps, and the blank lands on the last row. */
	int high = force || cur->rows > prev->rows ? cur->rows : prev->rows;
	if (high > FRAME_ROWS)
		high = FRAME_ROWS;

	if (force) {
		memcpy(out, "\033[2J\033[3J", 8);
		o = 8;
	}

	for (int i = 0; i < high; i++) {
		const char *want = i < cur->rows ? cur->line[i] : "";
		const char *have = i < prev->rows ? prev->line[i] : NULL;

		if (!force && have != NULL && strcmp(want, have) == 0)
			continue;

		char pos[32];
		int pn = snprintf(pos, sizeof pos, "\033[%d;1H\033[2K", i + 1);
		size_t wn = strlen(want);
		if (pn < 0)
			continue;

		if (o + (size_t)pn + wn + 1 >= sizeof out) {
			if (write(fd, out, o) < 0)
				return total;
			total += (long)o;
			o = 0;
		}
		if ((size_t)pn + wn + 1 >= sizeof out)
			continue;

		memcpy(out + o, pos, (size_t)pn);
		o += (size_t)pn;
		memcpy(out + o, want, wn);
		o += wn;
	}

	if (o > 0) {
		if (write(fd, out, o) < 0)
			return total;
		total += (long)o;
	}

	render_copy(prev, cur);
	return total;
}

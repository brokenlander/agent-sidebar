#ifndef CS_RENDER_H
#define CS_RENDER_H

#include "agents.h"

#define FRAME_ROWS 256
#define LINE_MAX 1024

typedef struct {
	char line[FRAME_ROWS][LINE_MAX];
	int row_agent[FRAME_ROWS];        /* agent index shown on that row, else -1 */
	char row_session[FRAME_ROWS][64]; /* session shown on that row, else "" */
	char row_sid[FRAME_ROWS][16];     /* and its id, the click's target */
	int keys_row;                     /* the shortcuts button, else -1 */
	int rows;
} frame;

/* The colour a status is drawn in, so other output can match the sidebar. */
const char *status_colour_for(agent_status s);
const char *frozen_colour(void);

/* self_sess: the session the sidebar runs in; its agent is drawn as current.
   sessions/nsessions: tmux sessions with no agent, listed below the agents.
   keys: draw the shortcuts button on the last row; rows of FRAME_ROWS means
   unbounded (--once), and the button then follows the list. */
void render_build(frame *f, const agent *a, int n, int cols, int rows,
		  long long now_ms, const char *self_sess,
		  const tsession *sessions, int nsessions, int keys);

/* "12s", "4m", "3h", "2d"; "-" when never seen. */
void render_age(char *dst, size_t cap, long long now_ms, long long seen_ms);

/* Emits only the lines that differ from prev, then copies cur into prev.
   force repaints everything (use after SIGWINCH). Returns bytes written. */
long render_flush(int fd, const frame *cur, frame *prev, int force);

#endif

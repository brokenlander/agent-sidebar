#ifndef CS_RENDER_H
#define CS_RENDER_H

#include "agents.h"

#define FRAME_ROWS 256
#define LINE_MAX 1024

typedef struct {
	char line[FRAME_ROWS][LINE_MAX];
	int row_agent[FRAME_ROWS];        /* agent index shown on that row, else -1 */
	char row_session[FRAME_ROWS][64]; /* session to jump to on click, else "" */
	int rows;
} frame;

/* The colour a status is drawn in, so other output can match the sidebar. */
const char *status_colour_for(agent_status s);

/* self_sess: the session the sidebar runs in; its agent is drawn as current.
   sessions/nsessions: tmux sessions with no agent, listed below the agents.
   legend: "key label;key label;..." shown as a key cheatsheet at the bottom.
   Either extra section is skipped when empty. */
void render_build(frame *f, const agent *a, int n, int cols, int rows,
		  long long now_ms, const char *self_sess,
		  char (*sessions)[64], int nsessions, const char *legend);

/* Emits only the lines that differ from prev, then copies cur into prev.
   force repaints everything (use after SIGWINCH). Returns bytes written. */
long render_flush(int fd, const frame *cur, frame *prev, int force);

#endif

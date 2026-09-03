#ifndef CS_RENDER_H
#define CS_RENDER_H

#include "agents.h"

#define FRAME_ROWS 256
#define LINE_MAX 1024

typedef struct {
	char line[FRAME_ROWS][LINE_MAX];
	int row_agent[FRAME_ROWS]; /* agent index shown on that row, else -1 */
	int rows;
} frame;

/* The colour a status is drawn in, so other output can match the sidebar. */
const char *status_colour_for(agent_status s);

/* self_sess: the session the sidebar runs in; its agent is drawn as current. */
void render_build(frame *f, const agent *a, int n, int cols, int rows,
		  long long now_ms, const char *self_sess);

/* Emits only the lines that differ from prev, then copies cur into prev.
   force repaints everything (use after SIGWINCH). Returns bytes written. */
long render_flush(int fd, const frame *cur, frame *prev, int force);

#endif

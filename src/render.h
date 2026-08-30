#ifndef CS_RENDER_H
#define CS_RENDER_H

#include "agents.h"

#define FRAME_ROWS 256
#define LINE_MAX 1024

typedef struct {
	char line[FRAME_ROWS][LINE_MAX];
	int rows;
} frame;

void render_build(frame *f, const agent *a, int n, int cols, int rows,
		  long long now_ms);

/* Emits only the lines that differ from prev, then copies cur into prev.
   force repaints everything (use after SIGWINCH). Returns bytes written. */
long render_flush(int fd, const frame *cur, frame *prev, int force);

#endif

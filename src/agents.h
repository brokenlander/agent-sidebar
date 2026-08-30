#ifndef CS_AGENTS_H
#define CS_AGENTS_H

#define AGENT_MAX 256

typedef enum {
	ST_WAITING = 0, /* needs you */
	ST_IDLE,        /* done, your turn */
	ST_SHELL,       /* dropped to a shell */
	ST_BUSY,        /* working, leave it */
	ST_UNKNOWN
} agent_status;

typedef struct {
	long long pid;
	agent_status status;
	char name[64];
	char cwd[256];   /* $HOME collapsed to ~ */
	char pane[64];   /* tmux target "sess:@win.%pane", empty if not in tmux */
	char sess[64];   /* tmux session name alone */
	long long seen_ms;
} agent;

/* Live interactive agents from $CLAUDE_CONFIG_DIR/sessions (default
   ~/.claude/sessions), sorted so what needs you sits at the top.
   Returns the count, or -1 when the directory cannot be read. */
int agents_load(agent *out, int cap);

const char *agent_status_label(agent_status s);

#endif

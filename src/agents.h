#ifndef CS_AGENTS_H
#define CS_AGENTS_H

#define AGENT_MAX 256

typedef enum {
	ST_WAITING = 0, /* needs you */
	ST_IDLE,        /* done, your turn - a backgrounded shell counts as idle */
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
	char pane_id[32];
	char agent[24];  /* producer: "opencode", empty for Claude Code */ /* "%27" - a complete tmux target on its own */
	long long seen_ms;
	int parked;      /* muted by the user: sorts last, dimmed */
} agent;

/* Live interactive agents from $CLAUDE_CONFIG_DIR/sessions (default
   ~/.claude/sessions), sorted so what needs you sits at the top.
   Returns the count, or -1 when the directory cannot be read. */
int agents_load(agent *out, int cap);

const char *agent_status_label(agent_status s);

/* Park list: tmux session names the user has muted, one per line, in
   $XDG_STATE_HOME/agent-sidebar/parked (default ~/.local/state/...). */
void agents_park_toggle(const char *sess);

/* True when the name is on the park list, whether or not it is running. */
int agents_park_has(const char *sess);

/* Drop the cached pane map so the next load re-reads it from tmux. */
void agents_invalidate(void);

#endif

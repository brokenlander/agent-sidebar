#ifndef CS_AGENTS_H
#define CS_AGENTS_H

#include <stddef.h>

#define AGENT_MAX 256

typedef enum {
	ST_ASKING = 0,  /* blocked on you right now: a prompt or dialog is up */
	ST_WAITING,     /* nobody blocked it, you just forgot it (see idle_wait) */
	ST_IDLE,        /* done, your turn - a backgrounded shell counts as idle */
	ST_BUSY,        /* working, leave it */
	ST_UNKNOWN
} agent_status;

typedef struct {
	long long pid;
	agent_status status;
	char name[64];
	char cwd[256];     /* $HOME collapsed to ~ */
	char sess[64];     /* tmux session name, empty if not in tmux */
	char pane_id[16];  /* "%27" - a complete tmux target on its own */
	char agent[24];    /* producer: "opencode", empty for Claude Code */
	long long seen_ms;
	int parked;        /* muted by the user: sorts last, dimmed */
	int frozen;        /* set aside for later: sorts below the live ones, blue */
} agent;

/* A tmux session: the name to show, the id ("$3") to target. */
typedef struct {
	char name[64];
	char id[16];
} tsession;

/* Live interactive agents from $CLAUDE_CONFIG_DIR/sessions (default
   ~/.claude/sessions) and the plugin directory, newest change first; frozen
   ones below the live ones, parked ones last. A missing directory is no
   agents. Returns the count. */
int agents_load(agent *out, int cap);

const char *agent_status_label(agent_status s);

/* An agent idle longer than this has stopped being "your turn" and started
   being "you forgot about me": it is reported as ST_WAITING. Seconds; 0
   disables the promotion. This is the common case by far - a reported
   ST_ASKING needs the agent to be blocked on input at that moment. */
void agents_set_idle_wait(long long secs);

/* Park and freeze lists: tmux session names, one per line, in
   $XDG_STATE_HOME/agent-sidebar/parked and .../frozen (default
   ~/.local/state/...). Parked is muted; frozen is kept for later. */
void agents_park_toggle(const char *sess);
void agents_freeze_toggle(const char *sess);

/* True when the name is on that list, whether or not it is running. */
int agents_park_has(const char *sess);
int agents_freeze_has(const char *sess);

/* Fetch the shared pane map from tmux now (after a rename, from the hooks). */
void agents_refresh_panes(void);

/* The shared pane map's file. */
const char *agents_map_path(void);

/* Session name of the pane the sidebar runs in, from the pane map. Empty
   when unknown. Returns 1 when found. */
int agents_self_session(char *dst, size_t cap);

/* tmux sessions with no agent in them, from the pane map, sorted by name.
   Call after agents_load so the map is current. Returns the count. */
int agents_sessions(tsession *out, int cap, const agent *a, int n,
		    const char *exclude);

/* $XDG_STATE_HOME/agent-sidebar/<leaf> (default ~/.local/state/...). */
void agents_state_path(char *dst, size_t cap, const char *leaf);
void mkdir_p(const char *path);

/* Run tmux with argv, no shell; capture stdout, return its exit status. */
int tmux_capture(char *const argv[], char *out, size_t cap);

long long now_ms(void);

#endif

/* Exercised under -fsanitize=address,undefined by `make test`. */
#define _POSIX_C_SOURCE 200809L

#include "../src/agents.h"
#include "../src/json.h"
#include "../src/render.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("  FAIL  %s\n", what);
		fails++;
	}
}

static void t_scalars(void)
{
	const char *s = "{\"a\":\"x\",\"n\":42,\"neg\":-7,\"t\":true,"
			"\"z\":null,\"q\":\"123\"}";
	jobj o;
	char buf[32];

	check(json_parse(s, strlen(s), &o) == 0, "flat object parses");
	check(o.n == 6, "six keys");
	check(json_string(json_get(&o, "a"), buf, sizeof buf) == 0 &&
	      strcmp(buf, "x") == 0, "string value");
	check(json_int(json_get(&o, "n"), -1) == 42, "number value");
	check(json_int(json_get(&o, "neg"), 0) == -7, "negative number");
	check(json_is_null(json_get(&o, "z")), "null detected");
	check(json_get(&o, "missing") == NULL, "absent key is NULL");
	check(json_int(json_get(&o, "q"), -1) == 123, "quoted digits");
}

static void t_nested_skipped(void)
{
	/* peerFeatures is an array in the real files; it must not desync
	   the scan of the keys that follow it. */
	const char *s = "{\"pre\":1,\"arr\":[\"a\",{\"b\":[1,2]}],"
			"\"obj\":{\"k\":\"}\"},\"post\":\"tail\"}";
	jobj o;
	char buf[32];

	check(json_parse(s, strlen(s), &o) == 0, "nested object parses");
	check(json_int(json_get(&o, "pre"), -1) == 1, "key before nesting");
	check(json_string(json_get(&o, "post"), buf, sizeof buf) == 0 &&
	      strcmp(buf, "tail") == 0, "key after nesting");
}

static void t_escapes(void)
{
	const char *s = "{\"a\":\"x\\\"y\",\"b\":\"tab\\there\","
			"\"c\":\"\\u00e9\",\"d\":\"\\ud83d\\ude00\","
			"\"e\":\"back\\\\slash\"}";
	jobj o;
	char buf[32];

	check(json_parse(s, strlen(s), &o) == 0, "escaped strings parse");
	check(json_string(json_get(&o, "a"), buf, sizeof buf) == 0 &&
	      strcmp(buf, "x\"y") == 0, "escaped quote");
	check(json_string(json_get(&o, "b"), buf, sizeof buf) == 0 &&
	      strcmp(buf, "tab\there") == 0, "escaped tab");
	check(json_string(json_get(&o, "c"), buf, sizeof buf) == 0 &&
	      strcmp(buf, "\xc3\xa9") == 0, "\\u -> utf8");
	check(json_string(json_get(&o, "d"), buf, sizeof buf) == 0 &&
	      strcmp(buf, "\xf0\x9f\x98\x80") == 0, "surrogate pair");
	check(json_string(json_get(&o, "e"), buf, sizeof buf) == 0 &&
	      strcmp(buf, "back\\slash") == 0, "escaped backslash");
}

static void t_malformed(void)
{
	const char *bad[] = {
		"", "{", "{\"a\"", "{\"a\":}", "{\"a\":1,", "{\"a\":\"unterminated",
		"[]", "{\"a\":[1,2}", "{\"a\":\"\\u00\"}", "not json at all",
	};
	jobj o;

	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		/* the contract is only "does not crash or read out of bounds";
		   ASan enforces the second half */
		int r = json_parse(bad[i], strlen(bad[i]), &o);
		(void)r;
	}
	check(1, "malformed input survives (ASan/UBSan clean)");
}

static void t_truncation(void)
{
	const char *s = "{\"a\":\"abcdefghijklmnop\"}";
	jobj o;
	char small[5];

	check(json_parse(s, strlen(s), &o) == 0, "parses for truncation test");
	check(json_string(json_get(&o, "a"), small, sizeof small) == 0,
	      "truncating copy succeeds");
	check(strlen(small) == 4, "truncated to cap-1");
}

/* Every real session file on this machine must parse. */
static void t_real_files(void)
{
	const char *home = getenv("HOME");
	char dir[512];
	int seen = 0, parsed = 0;

	snprintf(dir, sizeof dir, "%s/.claude/sessions", home ? home : ".");
	DIR *d = opendir(dir);
	if (d == NULL) {
		printf("  SKIP  real session files (%s unreadable)\n", dir);
		return;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);
		if (n < 6 || strcmp(e->d_name + n - 5, ".json") != 0)
			continue;

		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "rb");
		if (f == NULL)
			continue;

		static char buf[65536];
		size_t len = fread(buf, 1, sizeof buf, f);
		fclose(f);
		seen++;

		jobj o;
		if (json_parse(buf, len, &o) == 0)
			parsed++;
		else
			printf("  FAIL  real file did not parse: %s\n", e->d_name);
	}
	closedir(d);

	printf("  ....  %d/%d real session files parsed\n", parsed, seen);
	check(seen > 0 && parsed == seen, "every real session file parses");
}

/* The producer writes this file; the reader must agree on every field it
   depends on, or an opencode row silently vanishes. */
static void t_producer_fixture(void)
{
	FILE *f = fopen("tests/fixtures/opencode-live.json", "rb");
	if (f == NULL) {
		printf("  SKIP  producer fixture (not found)\n");
		return;
	}

	static char buf[65536];
	size_t len = fread(buf, 1, sizeof buf, f);
	fclose(f);

	jobj o;
	char s[64];
	check(json_parse(buf, len, &o) == 0, "producer fixture parses");
	check(json_int(json_get(&o, "pid"), -1) > 0, "fixture has a pid");
	check(json_int(json_get(&o, "procStart"), -1) >= 0,
	      "fixture has procStart the reader can compare");
	check(json_string(json_get(&o, "status"), s, sizeof s) == 0,
	      "fixture has a status");
	check(json_string(json_get(&o, "kind"), s, sizeof s) == 0 &&
	      strcmp(s, "interactive") == 0, "fixture kind is interactive");
	check(json_string(json_get(&o, "cwd"), s, sizeof s) == 0, "fixture has cwd");
}

static void t_render_widths(void)
{
	static agent a[3];
	static frame f;

	memset(a, 0, sizeof a);
	snprintf(a[0].name, sizeof a[0].name, "gateway-api-9f");
	snprintf(a[0].sess, sizeof a[0].sess, "csi");
	snprintf(a[0].cwd, sizeof a[0].cwd, "~/src/gateway-api");
	a[0].status = ST_BUSY;
	a[0].seen_ms = 1;
	snprintf(a[1].sess, sizeof a[1].sess, "a-very-long-session-name-here");
	a[1].status = ST_WAITING;
	a[2].status = ST_IDLE;

	/* narrow, normal, wide, and absurd - none may overflow a line */
	int widths[] = { 1, 8, 20, 42, 200, 4000 };
	for (size_t i = 0; i < sizeof widths / sizeof widths[0]; i++) {
		render_build(&f, a, 3, widths[i], 50, 1000000);
		for (int r = 0; r < f.rows; r++)
			check(strlen(f.line[r]) < LINE_MAX,
			      "rendered line stays in bounds");
	}
	render_build(&f, a, 0, 30, 50, 1000000);
	check(f.rows > 0, "empty agent list still renders");
}

int main(void)
{
	printf("agent-sidebar tests\n");
	t_scalars();
	t_nested_skipped();
	t_escapes();
	t_malformed();
	t_truncation();
	t_real_files();
	t_producer_fixture();
	t_render_widths();

	if (fails == 0) {
		printf("  OK    all checks passed\n");
		return 0;
	}
	printf("  %d check(s) failed\n", fails);
	return 1;
}

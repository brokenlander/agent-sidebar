#ifndef CS_JSON_H
#define CS_JSON_H

#include <stddef.h>

#define JSON_MAX_KEYS 48

typedef struct {
	const char *p;
	size_t n;
} jslice;

typedef struct {
	jslice key[JSON_MAX_KEYS];
	jslice val[JSON_MAX_KEYS];
	int n;
} jobj;

/* Parse one flat JSON object. Nested arrays/objects are captured as raw
   slices and never indexed into; the session files only nest in fields we
   do not read. Returns 0, or -1 on malformed input. */
int json_parse(const char *buf, size_t len, jobj *out);

/* NULL when the key is absent. */
const jslice *json_get(const jobj *o, const char *key);

/* Decode a JSON string into dst, always NUL-terminating, truncating at cap.
   Returns 0, or -1 when the value is not a string. */
int json_string(const jslice *v, char *dst, size_t cap);

/* These files carry only integral numbers; anything else yields dflt. */
long long json_int(const jslice *v, long long dflt);

int json_is_null(const jslice *v);

#endif

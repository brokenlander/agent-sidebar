#include "json.h"

#include <stdlib.h>
#include <string.h>

static void skip_ws(const char *b, size_t n, size_t *i)
{
	while (*i < n) {
		char c = b[*i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			(*i)++;
		else
			break;
	}
}

/* b[*i] must be '"'. Leaves *i just past the closing quote. */
static int scan_string(const char *b, size_t n, size_t *i)
{
	if (*i >= n || b[*i] != '"')
		return -1;
	(*i)++;
	while (*i < n) {
		char c = b[*i];
		if (c == '\\') {
			if (*i + 1 >= n)
				return -1;
			*i += 2;
			continue;
		}
		(*i)++;
		if (c == '"')
			return 0;
	}
	return -1;
}

/* Any value. Containers are skipped whole so nesting cannot desync the scan. */
static int scan_value(const char *b, size_t n, size_t *i)
{
	skip_ws(b, n, i);
	if (*i >= n)
		return -1;

	char c = b[*i];
	if (c == '"')
		return scan_string(b, n, i);

	if (c == '{' || c == '[') {
		int depth = 0;
		while (*i < n) {
			c = b[*i];
			if (c == '"') {
				if (scan_string(b, n, i) != 0)
					return -1;
				continue;
			}
			if (c == '{' || c == '[') {
				depth++;
			} else if (c == '}' || c == ']') {
				depth--;
				if (depth == 0) {
					(*i)++;
					return 0;
				}
			}
			(*i)++;
		}
		return -1;
	}

	/* number | true | false | null — runs to the next delimiter */
	while (*i < n) {
		c = b[*i];
		if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
		    c == '\n' || c == '\r')
			break;
		(*i)++;
	}
	return 0;
}

int json_parse(const char *b, size_t n, jobj *o)
{
	size_t i = 0;

	o->n = 0;
	skip_ws(b, n, &i);
	if (i >= n || b[i] != '{')
		return -1;
	i++;
	skip_ws(b, n, &i);
	if (i < n && b[i] == '}')
		return 0;

	for (;;) {
		skip_ws(b, n, &i);
		size_t ks = i;
		if (scan_string(b, n, &i) != 0)
			return -1;
		/* the slice excludes both quotes */
		jslice k = { b + ks + 1, (i - ks) - 2 };

		skip_ws(b, n, &i);
		if (i >= n || b[i] != ':')
			return -1;
		i++;

		skip_ws(b, n, &i);
		size_t vs = i;
		if (scan_value(b, n, &i) != 0)
			return -1;
		jslice v = { b + vs, i - vs };

		if (o->n < JSON_MAX_KEYS) {
			o->key[o->n] = k;
			o->val[o->n] = v;
			o->n++;
		}

		skip_ws(b, n, &i);
		if (i < n && b[i] == ',') {
			i++;
			continue;
		}
		if (i < n && b[i] == '}')
			return 0;
		return -1;
	}
}

const jslice *json_get(const jobj *o, const char *key)
{
	size_t klen = strlen(key);

	for (int i = 0; i < o->n; i++) {
		if (o->key[i].n == klen &&
		    memcmp(o->key[i].p, key, klen) == 0)
			return &o->val[i];
	}
	return NULL;
}

int json_is_null(const jslice *v)
{
	return v == NULL || (v->n == 4 && memcmp(v->p, "null", 4) == 0);
}

/* Append one code point as UTF-8. Returns bytes written, 0 if it would not fit. */
static size_t utf8_put(char *dst, size_t cap, unsigned long cp)
{
	if (cp < 0x80) {
		if (cap < 1)
			return 0;
		dst[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		if (cap < 2)
			return 0;
		dst[0] = (char)(0xC0 | (cp >> 6));
		dst[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		if (cap < 3)
			return 0;
		dst[0] = (char)(0xE0 | (cp >> 12));
		dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (cap < 4)
		return 0;
	dst[0] = (char)(0xF0 | (cp >> 18));
	dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	dst[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static int hex4(const char *p, unsigned long *out)
{
	unsigned long v = 0;

	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned long)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned long)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned long)(c - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

int json_string(const jslice *v, char *dst, size_t cap)
{
	if (cap == 0)
		return -1;
	dst[0] = '\0';
	if (v == NULL || v->n < 2 || v->p[0] != '"')
		return -1;

	const char *s = v->p + 1;
	size_t n = v->n - 2; /* strip the quotes */
	size_t o = 0;

	for (size_t i = 0; i < n && o + 1 < cap;) {
		char c = s[i];
		if (c != '\\') {
			dst[o++] = c;
			i++;
			continue;
		}
		if (i + 1 >= n)
			break;
		char e = s[i + 1];
		i += 2;
		switch (e) {
		case 'n': dst[o++] = '\n'; break;
		case 't': dst[o++] = '\t'; break;
		case 'r': dst[o++] = '\r'; break;
		case 'b': dst[o++] = '\b'; break;
		case 'f': dst[o++] = '\f'; break;
		case '"': dst[o++] = '"'; break;
		case '\\': dst[o++] = '\\'; break;
		case '/': dst[o++] = '/'; break;
		case 'u': {
			unsigned long cp;
			if (i + 4 > n || hex4(s + i, &cp) != 0)
				return -1;
			i += 4;
			/* surrogate pair */
			if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n &&
			    s[i] == '\\' && s[i + 1] == 'u') {
				unsigned long lo;
				if (hex4(s + i + 2, &lo) == 0 && lo >= 0xDC00 &&
				    lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) +
					     (lo - 0xDC00);
					i += 6;
				}
			}
			size_t w = utf8_put(dst + o, cap - 1 - o, cp);
			if (w == 0)
				goto done;
			o += w;
			break;
		}
		default:
			dst[o++] = e;
			break;
		}
	}
done:
	dst[o] = '\0';
	return 0;
}

long long json_int(const jslice *v, long long dflt)
{
	char buf[32];

	if (v == NULL || v->n == 0)
		return dflt;

	const char *p = v->p;
	size_t n = v->n;

	/* procStart and friends arrive as quoted digits */
	if (p[0] == '"') {
		if (n < 2)
			return dflt;
		p++;
		n -= 2;
	}
	if (n == 0 || n >= sizeof(buf))
		return dflt;

	memcpy(buf, p, n);
	buf[n] = '\0';

	char *end = NULL;
	long long r = strtoll(buf, &end, 10);
	if (end == buf || *end != '\0')
		return dflt;
	return r;
}

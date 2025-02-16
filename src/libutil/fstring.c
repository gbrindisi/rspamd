/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "fstring.h"
#include "str_util.h"

static const gsize default_initial_size = 48;
/* Maximum size when we double the size of new string */
static const gsize max_grow = 1024 * 1024;

#define fstravail(s) ((s)->allocated - (s)->len)
static rspamd_fstring_t * rspamd_fstring_grow (rspamd_fstring_t *str,
		gsize needed_len) G_GNUC_WARN_UNUSED_RESULT;

rspamd_fstring_t *
rspamd_fstring_new (void)
{
	rspamd_fstring_t *s;

	g_assert (posix_memalign ((void**)&s, 16, default_initial_size + sizeof (*s)) == 0);
	s->len = 0;
	s->allocated = default_initial_size;

	return s;
}

rspamd_fstring_t *
rspamd_fstring_sized_new (gsize initial_size)
{
	rspamd_fstring_t *s;
	gsize real_size = MAX (default_initial_size, initial_size);

	g_assert (posix_memalign ((void **)&s, 16, real_size + sizeof (*s)) == 0);
	s->len = 0;
	s->allocated = real_size;

	return s;
}

rspamd_fstring_t *
rspamd_fstring_new_init (const gchar *init, gsize len)
{
	rspamd_fstring_t *s;
	gsize real_size = MAX (default_initial_size, len);

	g_assert (posix_memalign ((void **) &s, 16, real_size + sizeof (*s)) == 0);
	s->len = len;
	s->allocated = real_size;
	memcpy (s->str, init, len);

	return s;
}

rspamd_fstring_t *
rspamd_fstring_assign (rspamd_fstring_t *str, const gchar *init, gsize len)
{
	gsize avail = fstravail (str);

	if (avail < len) {
		str = rspamd_fstring_grow (str, len);
	}

	if (len > 0) {
		memcpy (str->str, init, len);
	}

	str->len = len;

	return str;
}

void
rspamd_fstring_free (rspamd_fstring_t *str)
{
	free (str);
}

static rspamd_fstring_t *
rspamd_fstring_grow (rspamd_fstring_t *str, gsize needed_len)
{
	gsize newlen;
	gpointer nptr;

	newlen = str->allocated;

	/*
	 * Stop exponential grow at some point, since it might be slow for the
	 * vast majority of cases
	 */
	if (newlen < max_grow) {
		newlen *= 2;
	}
	else {
		newlen += max_grow;
	}

	/*
	 * Check for overflow
	 */
	if (newlen <= str->len + needed_len) {
		newlen = str->len + needed_len;

		if (newlen < max_grow) {
			newlen *= 2;
		}
		else {
			newlen += max_grow;
		}
	}

	nptr = realloc (str, newlen + sizeof (*str));

	if (nptr == NULL) {
		/* Avoid memory leak */
		free (str);
		g_assert (nptr);
	}

	str = nptr;
	str->allocated = newlen;

	return str;
}

rspamd_fstring_t *
rspamd_fstring_append (rspamd_fstring_t *str, const char *in, gsize len)
{
	gsize avail = fstravail (str);

	if (avail < len) {
		str = rspamd_fstring_grow (str, len);
	}

	memcpy (str->str + str->len, in, len);
	str->len += len;

	return str;
}

rspamd_fstring_t *
rspamd_fstring_append_chars (rspamd_fstring_t *str,
		char c, gsize len)
{
	gsize avail = fstravail (str);

	if (avail < len) {
		str = rspamd_fstring_grow (str, len);
	}

	memset (str->str + str->len, c, len);
	str->len += len;

	return str;
}

void
rspamd_fstring_erase (rspamd_fstring_t *str, gsize pos, gsize len)
{
	if (pos < str->len) {
		if (pos + len > str->len) {
			len = str->len - pos;
		}

		if (len == str->len - pos) {
			/* Fast path */
			str->len = pos;
		}
		else {
			memmove (str->str + pos, str->str + pos + len, str->len - pos);
			str->len -= pos;
		}
	}
	else {
		/* Do nothing */
	}
}

char *rspamd_fstring_cstr (const rspamd_fstring_t *str);

/* Compat code */
static guint32
fstrhash_c (gchar c, guint32 hval)
{
	guint32 tmp;
	/*
	 * xor in the current byte against each byte of hval
	 * (which alone gaurantees that every bit of input will have
	 * an effect on the output)
	 */
	tmp = c & 0xFF;
	tmp = tmp | (tmp << 8) | (tmp << 16) | (tmp << 24);
	hval ^= tmp;

	/* add some bits out of the middle as low order bits */
	hval = hval + ((hval >> 12) & 0x0000ffff);

	/* swap most and min significative bytes */
	tmp = (hval << 24) | ((hval >> 24) & 0xff);
	/* zero most and min significative bytes of hval */
	hval &= 0x00ffff00;
	hval |= tmp;
	/*
	 * rotate hval 3 bits to the left (thereby making the
	 * 3rd msb of the above mess the hsb of the output hash)
	 */
	return (hval << 3) + (hval >> 29);
}


/*
 * Return hash value for a string
 */
guint32
rspamd_fstrhash_lc (const rspamd_ftok_t * str, gboolean is_utf)
{
	gsize i;
	guint32 j, hval;
	const gchar *p, *end = NULL;
	gchar t;
	gunichar uc;

	if (str == NULL) {
		return 0;
	}

	p = str->begin;
	hval = str->len;

	if (is_utf) {
		while (end < str->begin + str->len) {
			if (!g_utf8_validate (p, str->len, &end)) {
				return rspamd_fstrhash_lc (str, FALSE);
			}
			while (p < end) {
				uc = g_unichar_tolower (g_utf8_get_char (p));
				for (j = 0; j < sizeof (gunichar); j++) {
					t = (uc >> (j * 8)) & 0xff;
					if (t != 0) {
						hval = fstrhash_c (t, hval);
					}
				}
				p = g_utf8_next_char (p);
			}
			p = end + 1;
		}

	}
	else {
		for (i = 0; i < str->len; i++, p++) {
			hval = fstrhash_c (g_ascii_tolower (*p), hval);
		}
	}

	return hval;
}

gboolean
rspamd_fstring_equal (const rspamd_fstring_t *s1,
		const rspamd_fstring_t *s2)
{
	g_assert (s1 != NULL && s2 != NULL);

	if (s1->len == s2->len) {
		return (memcmp (s1->str, s2->str, s1->len) == 0);
	}

	return FALSE;
}

gint
rspamd_fstring_casecmp (const rspamd_fstring_t *s1,
		const rspamd_fstring_t *s2)
{
	gint ret = 0;

	g_assert (s1 != NULL && s2 != NULL);

	if (s1->len == s2->len) {
		ret = rspamd_lc_cmp (s1->str, s2->str, s1->len);
	}
	else {
		ret = s1->len - s2->len;
	}

	return ret;
}

gint
rspamd_fstring_cmp (const rspamd_fstring_t *s1,
		const rspamd_fstring_t *s2)
{
	g_assert (s1 != NULL && s2 != NULL);

	if (s1->len == s2->len) {
		return memcmp (s1->str, s2->str, s1->len);
	}

	return s1->len - s2->len;
}

gint
rspamd_ftok_casecmp (const rspamd_ftok_t *s1,
		const rspamd_ftok_t *s2)
{
	gint ret = 0;

	g_assert (s1 != NULL && s2 != NULL);

	if (s1->len == s2->len) {
		ret = rspamd_lc_cmp (s1->begin, s2->begin, s1->len);
	}
	else {
		ret = s1->len - s2->len;
	}

	return ret;
}

gint
rspamd_ftok_cmp (const rspamd_ftok_t *s1,
		const rspamd_ftok_t *s2)
{
	g_assert (s1 != NULL && s2 != NULL);

	if (s1->len == s2->len) {
		return memcmp (s1->begin, s2->begin, s1->len);
	}

	return s1->len - s2->len;
}

void
rspamd_fstring_mapped_ftok_free (gpointer p)
{
	rspamd_ftok_t *tok = p;
	rspamd_fstring_t *storage;

	storage = (rspamd_fstring_t *) (tok->begin - 2 * sizeof (gsize));
	rspamd_fstring_free (storage);
	g_slice_free1 (sizeof (*tok), tok);
}

rspamd_ftok_t *
rspamd_ftok_map (const rspamd_fstring_t *s)
{
	rspamd_ftok_t *tok;

	g_assert (s != NULL);

	tok = g_slice_alloc (sizeof (*tok));
	tok->begin = s->str;
	tok->len = s->len;

	return tok;
}

char *
rspamd_fstring_cstr (const rspamd_fstring_t *s)
{
	char *result;

	if (s == NULL) {
		return NULL;
	}

	result = g_malloc (s->len + 1);
	memcpy (result, s->str, s->len);
	result[s->len] = '\0';

	return result;
}

gboolean
rspamd_ftok_cstr_equal (const rspamd_ftok_t *s, const gchar *pat,
		gboolean icase)
{
	gsize slen;
	rspamd_ftok_t srch;

	g_assert (s != NULL);
	g_assert (pat != NULL);

	slen = strlen (pat);
	srch.begin = pat;
	srch.len = slen;

	if (icase) {
		return (rspamd_ftok_casecmp (s, &srch) == 0);
	}

	return (rspamd_ftok_cmp (s, &srch) == 0);
}

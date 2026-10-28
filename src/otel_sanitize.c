/*-------------------------------------------------------------------------
 * otel_sanitize.c
 *	  Literal redaction for query text and plans.  Runs in the worker only.
 *
 * sql_mode = true  (db.query.text): strip comments (this removes the
 *     sqlcommenter payload), replace string / dollar-quoted / numeric
 *     literals with '?', collapse whitespace.
 * sql_mode = false (plans): only string literals are replaced; numbers, costs
 *     and layout are left alone.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "otel.h"

static inline bool
is_ident_char(unsigned char c)
{
	return isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

static inline void
put(StringInfo out, char c)
{
	appendStringInfoChar(out, c);
}

/* One space, never leading and never doubled. */
static inline void
put_space(StringInfo out)
{
	if (out->len > 0 && out->data[out->len - 1] != ' ')
		appendStringInfoChar(out, ' ');
}

void
otel_redact(StringInfo out, const char *s, int len, bool sql_mode)
{
	int			i = 0;

	while (i < len)
	{
		unsigned char c = (unsigned char) s[i];
		unsigned char prev = i > 0 ? (unsigned char) s[i - 1] : ' ';

		/* 'string', E'string' */
		if (c == '\'')
		{
			bool		esc = (prev == 'E' || prev == 'e') &&
				(i < 2 || !is_ident_char((unsigned char) s[i - 2]));
			int			j = i + 1;

			while (j < len)
			{
				if (esc && s[j] == '\\')
				{
					j += 2;
					continue;
				}
				if (s[j] == '\'')
				{
					if (j + 1 < len && s[j + 1] == '\'')
					{
						j += 2;
						continue;
					}
					break;
				}
				j++;
			}
			if (esc && out->len > 0)
				out->data[--out->len] = '\0';	/* drop the E prefix */
			put(out, '?');
			i = j + 1;
			continue;
		}

		/* "quoted identifier": keep verbatim */
		if (c == '"')
		{
			int			j = i + 1;

			while (j < len)
			{
				if (s[j] == '"')
				{
					if (j + 1 < len && s[j + 1] == '"')
					{
						j += 2;
						continue;
					}
					break;
				}
				j++;
			}
			appendBinaryStringInfo(out, s + i, Min(j + 1, len) - i);
			i = j + 1;
			continue;
		}

		/* $1 parameter, or $tag$ ... $tag$ */
		if (c == '$' && !is_ident_char(prev))
		{
			int			j = i + 1;

			if (j < len && isdigit((unsigned char) s[j]))
			{
				while (j < len && isdigit((unsigned char) s[j]))
					j++;
				appendBinaryStringInfo(out, s + i, j - i);
				i = j;
				continue;
			}
			while (j < len && (isalnum((unsigned char) s[j]) || s[j] == '_' ||
							   (unsigned char) s[j] >= 0x80) &&
				   !(j == i + 1 && isdigit((unsigned char) s[j])))
				j++;
			if (j < len && s[j] == '$')
			{
				int			taglen = j - i + 1;	/* includes both '$' */
				int			k = j + 1;

				while (k + taglen <= len && memcmp(s + k, s + i, taglen) != 0)
					k++;
				put(out, '?');
				i = (k + taglen <= len) ? k + taglen : len;
				continue;
			}
		}

		if (sql_mode)
		{
			/* block comment (PostgreSQL allows nesting) */
			if (c == '/' && i + 1 < len && s[i + 1] == '*')
			{
				int			depth = 1;

				i += 2;
				while (i < len && depth > 0)
				{
					if (s[i] == '/' && i + 1 < len && s[i + 1] == '*')
					{
						depth++;
						i += 2;
					}
					else if (s[i] == '*' && i + 1 < len && s[i + 1] == '/')
					{
						depth--;
						i += 2;
					}
					else
						i++;
				}
				put_space(out);
				continue;
			}
			/* line comment */
			if (c == '-' && i + 1 < len && s[i + 1] == '-')
			{
				while (i < len && s[i] != '\n')
					i++;
				put_space(out);
				continue;
			}
			/* numeric literal */
			if ((isdigit(c) || (c == '.' && i + 1 < len && isdigit((unsigned char) s[i + 1]))) &&
				!is_ident_char(prev) && prev != '.')
			{
				int			j = i + 1;
				bool		hex = (c == '0' && j < len && (s[j] == 'x' || s[j] == 'X'));

				while (j < len && (isalnum((unsigned char) s[j]) || s[j] == '_' || s[j] == '.' ||
								   (!hex && (s[j] == '+' || s[j] == '-') &&
									(s[j - 1] == 'e' || s[j - 1] == 'E'))))
					j++;
				put(out, '?');
				i = j;
				continue;
			}
			if (isspace(c))
			{
				put_space(out);
				i++;
				continue;
			}
		}

		put(out, (char) c);
		i++;
	}

	if (sql_mode)
	{
		while (out->len > 0 && out->data[out->len - 1] == ' ')
			out->data[--out->len] = '\0';
	}
}

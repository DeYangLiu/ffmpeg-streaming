#include "config.h"
#include "compact.h"
#include<stdio.h> //snprintf
#include <string.h> 
#include <stdarg.h>

int av_strstart(const char *str, const char *pfx, const char **ptr)
{
    while (*pfx && *pfx == *str) {
        pfx++;
        str++;
    }
    if (!*pfx && ptr)
        *ptr = str;
    return !*pfx;
}

int av_stristart(const char *str, const char *pfx, const char **ptr)
{
    while (*pfx && av_toupper((unsigned)*pfx) == av_toupper((unsigned)*str)) {
        pfx++;
        str++;
    }
    if (!*pfx && ptr)
        *ptr = str;
    return !*pfx;
}

char *av_stristr(const char *s1, const char *s2)
{
    if (!*s2)
        return (char*)(intptr_t)s1;

    do
        if (av_stristart(s1, s2, NULL))
            return (char*)(intptr_t)s1;
    while (*s1++);

    return NULL;
}

char *av_strnstr(const char *haystack, const char *needle, size_t hay_length)
{
    size_t needle_len = strlen(needle);
    if (!needle_len)
        return (char*)haystack;
    while (hay_length >= needle_len) {
        hay_length--;
        if (!memcmp(haystack, needle, needle_len))
            return (char*)haystack;
        haystack++;
    }
    return NULL;
}

size_t av_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = 0;
    while (++len < size && *src)
        *dst++ = *src++;
    if (len <= size)
        *dst = 0;
    return len + strlen(src) - 1;
}

size_t av_strlcat(char *dst, const char *src, size_t size)
{
    size_t len = strlen(dst);
    if (size <= len + 1)
        return len + strlen(src);
    return len + av_strlcpy(dst + len, src, size - len);
}

size_t av_strlcatf(char *dst, size_t size, const char *fmt, ...)
{
    size_t len = strlen(dst);
    va_list vl;

    va_start(vl, fmt);
    len += vsnprintf(dst + len, size > len ? size - len : 0, fmt, vl);
    va_end(vl);

    return len;
}

char *av_asprintf(const char *fmt, ...)
{
    char *p = NULL;
    va_list va;
    int len;

    va_start(va, fmt);
    len = vsnprintf(NULL, 0, fmt, va);
    va_end(va);
    if (len < 0)
        goto end;

    p = av_malloc(len + 1);
    if (!p)
        goto end;

    va_start(va, fmt);
    len = vsnprintf(p, len + 1, fmt, va);
    va_end(va);
    if (len < 0)
        av_freep(&p);

end:
    return p;
}

char *av_d2str(double d)
{
    char *str = av_malloc(16);
    if (str)
        snprintf(str, 16, "%f", d);
    return str;
}

#define WHITESPACES " \n\t"

char *av_get_token(const char **buf, const char *term)
{
    char *out     = av_malloc(strlen(*buf) + 1);
    char *ret     = out, *end = out;
    const char *p = *buf;
    if (!out)
        return NULL;
    p += strspn(p, WHITESPACES);

    while (*p && !strspn(p, term)) {
        char c = *p++;
        if (c == '\\' && *p) {
            *out++ = *p++;
            end    = out;
        } else if (c == '\'') {
            while (*p && *p != '\'')
                *out++ = *p++;
            if (*p) {
                p++;
                end = out;
            }
        } else {
            *out++ = c;
        }
    }

    do
        *out-- = 0;
    while (out >= end && strspn(out, WHITESPACES));

    *buf = p;

    return ret;
}

char *av_strtok(char *s, const char *delim, char **saveptr)
{
    char *tok;

    if (!s && !(s = *saveptr))
        return NULL;

    /* skip leading delimiters */
    s += strspn(s, delim);

    /* s now points to the first non delimiter char, or to the end of the string */
    if (!*s) {
        *saveptr = NULL;
        return NULL;
    }
    tok = s++;

    /* skip non delimiters */
    s += strcspn(s, delim);
    if (*s) {
        *s = 0;
        *saveptr = s+1;
    } else {
        *saveptr = NULL;
    }

    return tok;
}

int av_strcasecmp(const char *a, const char *b)
{
    uint8_t c1, c2;
    do {
        c1 = av_tolower(*a++);
        c2 = av_tolower(*b++);
    } while (c1 && c1 == c2);
    return c1 - c2;
}

int av_strncasecmp(const char *a, const char *b, size_t n)
{
    const char *end = a + n;
    uint8_t c1, c2;
    do {
        c1 = av_tolower(*a++);
        c2 = av_tolower(*b++);
    } while (a < end && c1 && c1 == c2);
    return c1 - c2;
}

int av_isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int av_isgraph(int c)
{
    return c > 32 && c < 127;
}

int av_isspace(int c)
{
    return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' ||
           c == '\v';
}

int av_isxdigit(int c)
{
    c = av_tolower(c);
    return av_isdigit(c) || (c >= 'a' && c <= 'f');
}

int av_match_name(const char *name, const char *names)
{
    const char *p;
    int len, namelen;

    if (!name || !names)
        return 0;

    namelen = strlen(name);
    while ((p = strchr(names, ','))) {
        len = FFMAX(p - names, namelen);
        if (!av_strncasecmp(name, names, len))
            return 1;
        names = p + 1;
    }
    return !av_strcasecmp(name, names);
}

int av_match_ext(const char *filename, const char *extensions)
{
    const char *ext;

    if (!filename)
        return 0;

    ext = strrchr(filename, '.');
    if (ext)
        return av_match_name(ext + 1, extensions);
    return 0;
}

#if HAVE_STRPTIME == 0
static int get_word(char *src, char *dst)
{
	char *p = src;
	while(*p && isspace(*p)){
		p++;
	}

	while(*p && !isspace(*p)){
		*dst++ = *p++;
	}
	*dst = 0;
	return p - src;
}

static time_t av_timegm(struct tm *tm)
{
    time_t t;

    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;

    if (m < 3) {
        m += 12;
        y--;
    }

    t = 86400LL *
        (d + (153 * m - 457) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 719469);

    t += 3600 * tm->tm_hour + 60 * tm->tm_min + tm->tm_sec;

    return t;
}

static int get_mon(char *m)
{
	static char *name[] = {
		"",
		"Jan", "Feb", "Mar",
		"Apr", "May", "Jun",
		"Jul", "Aug", "Sep",
		"Oct", "Nov", "Dec",
	};
	int i, n = sizeof(name)/sizeof(name[0]);
	for(i = 0; i < n; ++i){
		if(!strcmp(name[i], m)){
			return i;
		}
	}
	
	return 0;
}

char* ff_strptime(const char *str, const char *fmt, time_t *tt)
{
	char *p, word[64];
	struct tm tm = {0};
	if(strcmp(fmt, "%a, %d %b %Y %H:%M:%S GMT")){
		printf("not support fmt '%s'\n", fmt);
		return NULL;
	}

	p = (char*)str;
	p += get_word(p, word); //Week
	p += get_word(p, word); 
	tm.tm_mday = atoi(word);
	p += get_word(p, word);
	tm.tm_mon = get_mon(word) - 1;
	p += get_word(p, word);
	tm.tm_year = atoi(word) - 1900;

	p += get_word(p, word);
	sscanf(word, "%02d:%02d:%02d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
	*tt = av_timegm(&tm);
	p += get_word(p, word);
	return p;
}

#endif


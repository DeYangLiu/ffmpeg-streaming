#ifndef _COMPACT_H_
#define _COMPACT_H_
#include <stddef.h> //EDOM
#include <stdint.h>
#include <limits.h> //INT_MAX
#include <string.h>
#include <errno.h> //EAGAIN
#include <time.h> //time_t

#if EDOM > 0
#define AVERROR(e) (-(e))   ///< Returns a negative error code from a POSIX error code, to return from library functions.
#define AVUNERROR(e) (-(e)) ///< Returns a POSIX error code from a library function error return value.
#else
/* Some platforms have E* and errno already negated. */
#define AVERROR(e) (e)
#define AVUNERROR(e) (e)
#endif

#ifndef AV_RB32
#define AV_RB32(p) ( (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | (p[3] << 0) )
#endif

#ifndef FFABS
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#endif

#ifndef FFMAX
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef FFMIN
#define FFMIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef av_assert0
#define av_assert0(cond) if(!(cond)){printf("assertion failed at %s:%d\n",  __FILE__, __LINE__); abort();}
#endif

#ifndef av_const
#define av_const const
#endif

#if PLUGIN_DIR
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#endif

#if HAVE_WINSOCK2_H
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT       WSAETIMEDOUT
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED    WSAECONNREFUSED
#endif
#ifndef EINPROGRESS
#define EINPROGRESS     WSAEINPROGRESS
#endif

#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH

#define getsockopt(a, b, c, d, e) getsockopt(a, b, c, (char*) d, e)
#define setsockopt(a, b, c, d, e) setsockopt(a, b, c, (const char*) d, e)

int ff_neterrno(void);
#define close_socket(x) do{ /*shutdown(x, SHUT_RDWR);*/ closesocket(x);}while(0)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> //inet_ntoa
#include <netdb.h>
#include <netinet/tcp.h> //for TCP_NODELAY

#define close_socket close
#define ff_neterrno() AVERROR(errno)
#endif /* HAVE_WINSOCK2_H */

#if !HAVE_POLL_H
typedef unsigned long nfds_t;

#if HAVE_WINSOCK2_H
#include <winsock2.h>
#endif
#if !HAVE_STRUCT_POLLFD
struct pollfd {
    int fd;
    short events;  /* events to look for */
    short revents; /* events that occurred */
};

/* events & revents */
#define POLLIN     0x0001  /* any readable data available */
#define POLLOUT    0x0002  /* file descriptor is writeable */
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT
#define POLLRDBAND 0x0008  /* priority readable data */
#define POLLWRBAND 0x0010  /* priority data can be written */
#define POLLPRI    0x0020  /* high priority readable data */

/* revents only */
#define POLLERR    0x0004  /* errors pending */
#define POLLHUP    0x0080  /* disconnected */
#define POLLNVAL   0x1000  /* invalid file descriptor */
#endif


int ff_poll(struct pollfd *fds, nfds_t numfds, int timeout);
#define poll ff_poll
#endif /* HAVE_POLL_H */

void *av_malloc(size_t size);
void *av_mallocz(size_t size);
void av_freep(void *arg);
void *av_mallocz(size_t size);
void *av_realloc(void *ptr, size_t size);
void av_free(void *ptr);

static inline void *av_malloc_array(size_t nmemb, size_t size)
{
    if (!size || nmemb >= INT_MAX / size)
        return NULL;
    return av_malloc(nmemb * size);
}

static inline void *av_mallocz_array(size_t nmemb, size_t size)
{
    if (!size || nmemb >= INT_MAX / size)
        return NULL;
    return av_mallocz(nmemb * size);
}

/**
 * Locale-independent conversion of ASCII characters to uppercase.
 */
static inline av_const int av_toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        c ^= 0x20;
    return c;
}

/**
 * Locale-independent conversion of ASCII characters to lowercase.
 */
static inline av_const int av_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        c ^= 0x20;
    return c;
}

int64_t av_gettime_relative(void);
int av_usleep(unsigned usec);
int64_t av_gettime(void);
int av_gettime_relative_is_monotonic(void);

//prototypes
int ff_socket_nonblock(int socket, int enable);
int av_gettime_relative_is_monotonic(void);
int av_strstart(const char *str, const char *pfx, const char **ptr);
int av_stristart(const char *str, const char *pfx, const char **ptr);
char *av_stristr(const char *s1, const char *s2);
char *av_strnstr(const char *haystack, const char *needle, size_t hay_length);
size_t av_strlcpy(char *dst, const char *src, size_t size);
size_t av_strlcat(char *dst, const char *src, size_t size);
char *av_strtok(char *s, const char *delim, char **saveptr);
int av_strcasecmp(const char *a, const char *b);
int av_strncasecmp(const char *a, const char *b, size_t n);
int av_isdigit(int c);
int av_isgraph(int c);
int av_isspace(int c);
int av_isxdigit(int c);
int av_match_name(const char *name, const char *names);
int av_match_ext(const char *filename, const char *extensions);
size_t av_strlcatf(char *dst, size_t size, const char *fmt, ...);
char *av_asprintf(const char *fmt, ...);
char *av_d2str(double d);
char *av_get_token(const char **buf, const char *term);

#if HAVE_STRPTIME == 0
char* ff_strptime(const char *str, const char *fmt, time_t *tt);
#define strptime ff_strptime
#endif

#endif //_COMPACT_H_

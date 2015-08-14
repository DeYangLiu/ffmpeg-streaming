#include "config.h"
#include "compact.h"
#include <stdarg.h>
#include <stdlib.h>
#if HAVE_GETTIMEOFDAY
#include <sys/time.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_WINDOWS_H
#include <windows.h>
#else
#include <fcntl.h> //F_SETFL
#endif


#if HAVE_WINSOCK2_H
int ff_neterrno(void)
{
    int err = WSAGetLastError();
    switch (err) {
    case WSAEWOULDBLOCK:
        return AVERROR(EAGAIN);
    case WSAEINTR:
        return AVERROR(EINTR);
    case WSAEPROTONOSUPPORT:
        return AVERROR(EPROTONOSUPPORT);
    case WSAETIMEDOUT:
        return AVERROR(ETIMEDOUT);
    case WSAECONNREFUSED:
        return AVERROR(ECONNREFUSED);
    case WSAEINPROGRESS:
        return AVERROR(EINPROGRESS);
    }
    return -err;
}
#endif

int ff_socket_nonblock(int socket, int enable)
{
#if HAVE_WINSOCK2_H
    u_long param = enable;
    return ioctlsocket(socket, FIONBIO, &param);
#else
    if (enable)
        return fcntl(socket, F_SETFL, fcntl(socket, F_GETFL) | O_NONBLOCK);
    else
        return fcntl(socket, F_SETFL, fcntl(socket, F_GETFL) & ~O_NONBLOCK);
#endif /* HAVE_WINSOCK2_H */
}

#if !HAVE_POLL_H
int ff_poll(struct pollfd *fds, nfds_t numfds, int timeout)
{
    fd_set read_set;
    fd_set write_set;
    fd_set exception_set;
    nfds_t i;
    int n;
    int rc;

#if HAVE_WINSOCK2_H
    if (numfds >= FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
#endif /* HAVE_WINSOCK2_H */

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&exception_set);

    n = 0;
    for (i = 0; i < numfds; i++) {
        if (fds[i].fd < 0)
            continue;
#if !HAVE_WINSOCK2_H
        if (fds[i].fd >= FD_SETSIZE) {
            errno = EINVAL;
            return -1;
        }
#endif /* !HAVE_WINSOCK2_H */

        if (fds[i].events & POLLIN)
            FD_SET(fds[i].fd, &read_set);
        if (fds[i].events & POLLOUT)
            FD_SET(fds[i].fd, &write_set);
        if (fds[i].events & POLLERR)
            FD_SET(fds[i].fd, &exception_set);

        if (fds[i].fd >= n)
            n = fds[i].fd + 1;
    }

    if (n == 0)
        /* Hey!? Nothing to poll, in fact!!! */
        return 0;

    if (timeout < 0) {
        rc = select(n, &read_set, &write_set, &exception_set, NULL);
    } else {
        struct timeval tv;
        tv.tv_sec  = timeout / 1000;
        tv.tv_usec = 1000 * (timeout % 1000);
        rc         = select(n, &read_set, &write_set, &exception_set, &tv);
    }

    if (rc < 0)
        return rc;

    for (i = 0; i < numfds; i++) {
        fds[i].revents = 0;

        if (FD_ISSET(fds[i].fd, &read_set))
            fds[i].revents |= POLLIN;
        if (FD_ISSET(fds[i].fd, &write_set))
            fds[i].revents |= POLLOUT;
        if (FD_ISSET(fds[i].fd, &exception_set))
            fds[i].revents |= POLLERR;
    }

    return rc;
}
#endif /* !HAVE_POLL_H */


void *av_malloc(size_t size)
{
	return malloc(size);
}

void *av_mallocz(size_t size)
{
    void *ptr = av_malloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

void *av_realloc(void *ptr, size_t size)
{
	return realloc(ptr, size + !size);
}

void av_freep(void *arg)
{
    void *val;

    memcpy(&val, arg, sizeof(val));
    memcpy(arg, &(void *){ NULL }, sizeof(val));
    av_free(val);
}

void av_free(void *ptr)
{
	if(ptr)
		free(ptr);
}

int64_t av_gettime(void)
{
#if HAVE_GETTIMEOFDAY
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
#elif HAVE_GETSYSTEMTIMEASFILETIME
    FILETIME ft;
    int64_t t;
    GetSystemTimeAsFileTime(&ft);
    t = (int64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return t / 10 - 11644473600000000; /* Jan 1, 1601 */
#else
    return -1;
#endif
}

int64_t av_gettime_relative(void)
{
#if HAVE_CLOCK_GETTIME && defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
    return av_gettime() + 42 * 60 * 60 * INT64_C(1000000);
#endif
}

int av_gettime_relative_is_monotonic(void)
{
#if HAVE_CLOCK_GETTIME && defined(CLOCK_MONOTONIC)
    return 1;
#else
    return 0;
#endif
}

int av_usleep(unsigned usec)
{
#if HAVE_NANOSLEEP
    struct timespec ts = { usec / 1000000, usec % 1000000 * 1000 };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR);
    return 0;
#elif HAVE_USLEEP
    return usleep(usec);
#elif HAVE_SLEEP
    Sleep(usec / 1000);
    return 0;
#else
    return AVERROR(ENOSYS);
#endif
}





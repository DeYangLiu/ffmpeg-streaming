#ifndef _TIME_LOG_H_
#define _TIME_LOG_H_


#if HAVE_WINDOWS_H
#include <windows.h>
#else
#include <sys/time.h> //gettimeofday
#endif

#include <string.h>
#include <stdio.h>

static char *ctime1(char *buf2, int buf_size)
{
	#if HAVE_WINDOWS_H
	SYSTEMTIME local_time = { 0 };
	GetLocalTime(&local_time);
	snprintf(buf2, buf_size, "[%02u:%02u:%02u.%03u]", local_time.wHour, local_time.wMinute, local_time.wSecond, local_time.wMilliseconds);
	
	#elif HAVE_GETTIMEOFDAY
	struct timeval tpend;
	gettimeofday(&tpend,NULL);
							
	int secofday = (tpend.tv_sec + 3600 * 8 ) % 86400;
	int hours = secofday / 3600;
	int minutes = (secofday - hours * 3600 ) / 60;
	int seconds = secofday % 60;
	int milliseconds = tpend.tv_usec/1000;
	char buf[40];
	snprintf(buf2, buf_size, "[%02u:%02u:%02u.%03u]", hours, minutes, seconds, milliseconds);
	
	#elif 0
	int64_t t = av_gettime_relative()/1000;
	uint32_t h, m, s, ss;
	ss = t%1000; t = t/1000;
	s = t%60;  t = t/60;
	m = t%60;  t = t/60;
	h = t;
	snprintf(buf2, buf_size, "[%02u:%02u:%02u.%03u]", h, m, s, ss);
	#else
	buf2[0] = 0;
	#endif
	
	return buf2;
}

static void time_vlog(const char *fmt, va_list vargs)
{
    static int print_prefix = 1;
	FILE *logfile = stdout;
	
    if (logfile) {
        if (print_prefix) {
            char buf[32];
            ctime1(buf, sizeof(buf));
            fprintf(logfile, "%s ", buf);
        }
        print_prefix = strstr(fmt, "\n") != NULL;
        vfprintf(logfile, fmt, vargs);
        //fflush(logfile);
    }
}

//__attribute__ ((format (printf, 1, 2)))
static void time_log(const char *fmt, ...)
{
    va_list vargs;
    va_start(vargs, fmt);
    time_vlog(fmt, vargs);
    va_end(vargs);
}

#if FFMPEG_SRC
static void time_av_log(void *ptr, int level, const char *fmt, va_list vargs)
{
    static int print_prefix = 1;
	
    AVClass *avc = ptr ? *(AVClass**)ptr : NULL;
    if (level > av_log_get_level())
        return;
    if (print_prefix && avc)
        time_log("[%s @ %p]", avc->item_name(ptr), ptr);
    print_prefix = strstr(fmt, "\n") != NULL;
	
    time_vlog(fmt, vargs);
}
#endif

#endif //_TIME_LOG_H_

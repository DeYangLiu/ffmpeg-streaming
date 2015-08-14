/*
config file -- handwrited sample, can by overwrittten by configure.
*/
#ifndef _CONFIG_H_
#define _CONFIG_H_

#define HAVE_WINDOWS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_USLEEP 1

#define HAVE_GETTIMEOFDAY 1
#if HAVE_WINDOWS_H
#define HAVE_WINSOCK2_H 1
#define HAVE_POLL_H 0
#define HAVE_SLEEP 1
#define HAVE_NANOSLEEP 0
#define HAVE_GETSYSTEMTIMEASFILETIME 1
#else
#define HAVE_WINSOCK2_H 0
#define HAVE_POLL_H 1
#define HAVE_GETTIMEOFDAY 1
#endif


#endif //_CONFIG_H_

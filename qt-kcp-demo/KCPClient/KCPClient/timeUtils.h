#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <string.h>


#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#include <windows.h>
#elif !defined(__unix)
#define __unix
#endif

#ifdef __unix
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif

/* get system time */
static inline void itimeofday(long *sec, long *usec)
{
#if defined(__unix)
	struct timeval time;
	gettimeofday(&time, NULL);
	if (sec) *sec = time.tv_sec;
	if (usec) *usec = time.tv_usec;
#else
	static long mode = 0, addsec = 0;
	BOOL retval;
	static long long freq = 1;
	long long qpc;
	if (mode == 0) {
		retval = QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		freq = (freq == 0) ? 1 : freq;
		retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
		addsec = (long)time(NULL);
		addsec = addsec - (long)((qpc / freq) & 0x7fffffff);
		mode = 1;
	}
	retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
	retval = retval * 2;
	if (sec) *sec = (long)(qpc / freq) + addsec;
	if (usec) *usec = (long)((qpc % freq) * 1000000 / freq);
#endif
}

/* get clock in millisecond 64 */
static inline long long iclock64(void)
{
	long s, u;
	long long value;
	itimeofday(&s, &u);
	value = ((long long)s) * 1000 + (u / 1000);
	return value;
}

static inline unsigned int iclock()
{
	return (unsigned)(iclock64() & 0xfffffffful);
}

/* sleep in millisecond */
static inline void isleep(unsigned long millisecond)
{
#ifdef __unix 	/* usleep( time * 1000 ); */
	struct timespec ts;
	ts.tv_sec = (time_t)(millisecond / 1000);
	ts.tv_nsec = (long)((millisecond % 1000) * 1000000);
	/*nanosleep(&ts, NULL);*/
	usleep((millisecond << 10) - (millisecond << 4) - (millisecond << 3));
#elif defined(_WIN32)
	Sleep(millisecond);
#endif
}
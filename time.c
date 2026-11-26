#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <time.h>

#include "time.h"

int64_t getCurrentTime()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);

	return (int64_t)tv.tv_sec*1000000 + (int64_t)tv.tv_usec;
}

int64_t getCurrentTimeNs()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	return microsecondsToNanoseconds((int64_t)ts.tv_sec*1000000) + ts.tv_nsec;
}

int64_t microsecondsToNanoseconds(int64_t t)
{
	if (t > INT64_MAX / 1000) {
		return INT64_MAX;
	}
	if (t < INT64_MIN / 1000) {
		return INT64_MIN;
	}
	return t * 1000;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>

int64_t getCurrentTime()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);

	return (int64_t)tv.tv_sec*1000000 + (int64_t)tv.tv_usec;
}


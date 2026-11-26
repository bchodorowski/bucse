#include <stdint.h>

// the current time in microseconds since the epoch (the unit of action times)
int64_t getCurrentTime();

// the current time in nanoseconds since the epoch (the unit of atime and mtime)
int64_t getCurrentTimeNs();

// converts microseconds (e.g. an action time) to nanoseconds; a time out of
// the range of int64_t nanoseconds (years 1677 to 2262) is clamped to it
int64_t microsecondsToNanoseconds(int64_t t);

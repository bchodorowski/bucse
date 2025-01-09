#define LOG_ERROR 0
#define LOG_WARNING 1
#define LOG_NOTE 2
#define LOG_DEBUG 3
#define LOG_VERBOSE_DEBUG 4

int logPrintf(int verbosity, char* format, ...);

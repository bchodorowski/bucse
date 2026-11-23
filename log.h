#define LOG_ERROR 0
#define LOG_WARNING 1
#define LOG_NOTE 2
#define LOG_DEBUG 3
#define LOG_VERBOSE_DEBUG 4

#ifdef __GNUC__
#define LOG_PRINTF_FORMAT __attribute__((format(printf, 2, 3)))
#else
#define LOG_PRINTF_FORMAT
#endif

int logPrintf(int verbosity, char* format, ...) LOG_PRINTF_FORMAT;

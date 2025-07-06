#include <stdio.h>
#include <stdarg.h>

#include "conf.h"

static char* verbosityLabels[] = {
	"error", "warning", "note", "debug", "verbose_debug"};

int logPrintf(int verbosity, char* format, ...)
{
	if (verbosity > conf.verbose) {
		return 0;
	}

	fprintf(stdout, "[%s]: ", verbosityLabels[verbosity]);

	va_list ptr;

	va_start(ptr, format);
	int result = vfprintf(stdout, format, ptr);
	va_end(ptr);

	fflush(stdout);
	return result;
}


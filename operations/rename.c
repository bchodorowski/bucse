#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fuse.h>
#include <pthread.h>

#include "../dynarray.h"
#include "../filesystem.h"
#include "../actions.h"

#include "operations.h"

#include "rename.h"

static int bucse_rename(const char *srcPath, const char *dstPath,
		unsigned int flags)
{
	// not implemented yet
	return -ENOSYS;
}
	
int bucse_rename_guarded(const char *srcPath, const char *dstPath,
		unsigned int flags)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_rename(srcPath, dstPath, flags);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fuse.h>
#include <pthread.h>

#include "../dynarray.h"
#include "../filesystem.h"
#include "../actions.h"
#include "../log.h"

#include "operations.h"
#include "flush.h"

#include "release.h"

static int bucse_release(const char *path, struct fuse_file_info *fi)
{
	logPrintf(LOG_DEBUG, "release %s, access mode %d\n", path, fi->flags);

	if (path == NULL) {
		return -EIO;
	}

	if ((fi->flags & O_ACCMODE) == O_RDONLY) {
		return 0;
	}

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_release: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_release: path not found when releasing file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			flushFile(file);
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

int bucse_release_guarded(const char *path, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_release(path, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

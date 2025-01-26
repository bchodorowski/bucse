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

#include "write.h"

static int bucse_write(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	logPrintf(LOG_DEBUG, "write %s, size: %u, offset: %u\n", path, size, offset);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_write: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_write: path not found when writing file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	PendingWrite* newPendingWrite = malloc(sizeof(PendingWrite));
	if (newPendingWrite == NULL) {
		logPrintf(LOG_ERROR, "bucse_write: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newPendingWrite->size = size;
	newPendingWrite->buf = malloc(size);
	if (newPendingWrite->buf == NULL) {
		logPrintf(LOG_ERROR, "bucse_write: malloc(): %s\n", strerror(errno));
		free(newPendingWrite);
		return -ENOMEM;
	}
	memcpy(newPendingWrite->buf, buf, size);
	newPendingWrite->offset = offset;
	addToDynArray(&file->pendingWrites, newPendingWrite);
	file->dirtyFlags |= DirtyFlagPendingWrite;

	// TODO: flush file if pendingWrites are too large

	return size;
}

int bucse_write_guarded(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_write(path, buf, size, offset, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


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

#include "truncate.h"

static int bucse_truncate(const char *path, long int newSize, struct fuse_file_info *fi)
{
	(void) fi;

	logPrintf(LOG_DEBUG, "truncate %s, size: %ld\n", path, newSize);

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
			logPrintf(LOG_ERROR, "bucse_truncate: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_truncate: path not found when writing file %s\n", path);
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

	if (file->dirtyFlags != DirtyFlagNotDirty) {
		flushFile(file);
	}

	int size = file->size;
	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		if (size < (pw->offset + pw->size)) {
			size = (pw->offset + pw->size);
		}
	}

	if (newSize == size) {
		return 0;
	} else if (newSize > size) {
		PendingWrite* newPendingWrite = malloc(sizeof(PendingWrite));
		if (newPendingWrite == NULL) {
			logPrintf(LOG_ERROR, "bucse_truncate: malloc(): %s\n", strerror(errno));
			return -ENOMEM;
		}
		newPendingWrite->size = newSize - size;
		newPendingWrite->buf = malloc(newSize - size);
		if (newPendingWrite->buf == NULL) {
			logPrintf(LOG_ERROR, "bucse_truncate: malloc(): %s\n", strerror(errno));
			free(newPendingWrite);
			return -ENOMEM;
		}
		memset(newPendingWrite->buf, 0, newSize - size);
		newPendingWrite->offset = size;
		addToDynArray(&file->pendingWrites, newPendingWrite);
		file->dirtyFlags |= DirtyFlagPendingWrite;
		return 0;
	}

	// (newSize < size), so
	
	file->truncSize = newSize;
	file->dirtyFlags |= DirtyFlagPendingWrite;

	return 0;
}

int bucse_truncate_guarded(const char *path, long int newSize, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_truncate(path, newSize, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


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
#include "../conf.h"

#include "operations.h"
#include "flush.h"

#include "truncate.h"

static int bucse_truncate(const char *path, long int newSize, struct fuse_file_info *fi)
{
	logPrintf(LOG_DEBUG, "truncate %s, size: %ld\n", path, newSize);

	if (confIsReadOnly()) {
		logPrintf(LOG_ERROR, "bucse_truncate: cannot do that in readOnly mode\n");
		return -EROFS;
	}

	if (path == NULL) {
		return -EIO;
	}
	if (newSize < 0) {
		return -EINVAL;
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
		if (flushFile(file) != 0) {
			return -EIO;
		}
	}

	// the file isn't dirty any more (see above), so file->size is its size;
	// the flush also makes a shrink followed by an extend zero the bytes in
	// between (flushFile() keeps the old data only up to the last truncSize)
	if ((size_t)newSize == file->size) {
		return 0;
	}

	// an extending truncate is pending too, without a buffer: flushFile()
	// writes the blocks past the old size as zeroes, one block at a time
	size_t oldTruncSize = file->truncSize;
	DirtyFlags oldDirtyFlags = file->dirtyFlags;
	file->truncSize = newSize;
	file->dirtyFlags |= DirtyFlagPendingTrunc;

	// a truncate by path (truncate(2), no open handle) is flushed right away:
	// no release of a handle would flush it, so it would stay pending until
	// the file is flushed for another reason, or the unmount. If the flush
	// fails, the truncate is undone, so that it isn't applied later after
	// the error was returned.
	if (fi == NULL) {
		if (flushFile(file) != 0) {
			file->truncSize = oldTruncSize;
			file->dirtyFlags = oldDirtyFlags;
			return -EIO;
		}
	}

	return 0;
}

int bucse_truncate_guarded(const char *path, long int newSize, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_truncate(path, newSize, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


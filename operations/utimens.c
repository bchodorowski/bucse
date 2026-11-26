#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <fuse.h>
#include <pthread.h>

#include "../dynarray.h"
#include "../filesystem.h"
#include "../actions.h"
#include "../time.h"
#include "../log.h"
#include "../conf.h"

#include "operations.h"
#include "flush.h"

#include "utimens.h"

// Converts a time given to utimens to nanoseconds. Returns non-zero for
// UTIME_OMIT (*result isn't set), UTIME_NOW gives now. A time out of the
// range of int64_t nanoseconds gives -EOVERFLOW, an invalid tv_nsec -EINVAL.
static int getRequestedTime(const struct timespec* ts, int64_t now, int64_t* result)
{
	if (ts->tv_nsec == UTIME_OMIT) {
		return 1;
	}
	if (ts->tv_nsec == UTIME_NOW) {
		*result = now;
		return 0;
	}
	if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L) {
		return -EINVAL;
	}
	// ts->tv_sec * 1000000000 + ts->tv_nsec must fit in int64_t
	if (ts->tv_sec > (INT64_MAX - ts->tv_nsec) / 1000000000L
		|| ts->tv_sec < INT64_MIN / 1000000000L) {
		return -EOVERFLOW;
	}
	*result = (int64_t)ts->tv_sec * 1000000000L + ts->tv_nsec;
	return 0;
}

// Writes a setTimes action. A file with local changes is flushed first, so
// that the action applies to the file in the repository, and the editFile or
// addFile of the flush doesn't come after it (it would set the mtime to its
// time).
static int bucse_utimens(const char *path, const struct timespec tv[2],
	struct fuse_file_info *fi)
{
	(void) fi;

	logPrintf(LOG_DEBUG, "utimens %s\n", path);

	if (confIsReadOnly()) {
		logPrintf(LOG_ERROR, "bucse_utimens: cannot do that in readOnly mode\n");
		return -EROFS;
	}

	if (path == NULL || path[0] != '/') {
		return -ENOENT;
	}

	FilesystemFile* file = NULL;
	FilesystemDir* dir = NULL;
	if (strcmp(path, "/") == 0) {
		dir = root;
	} else {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_utimens: path_split() failed\n");
			return -ENOMEM;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			dir = findDir(containingDir, fileName);
			if (dir == NULL) {
				return -ENOENT;
			}
		}
	}

	// tv is NULL for utime(path, NULL) on older kernels: both times now
	struct timespec nowTimes[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
	if (tv == NULL) {
		tv = nowTimes;
	}
	int64_t atime, mtime;
	int hasAtime = getRequestedTime(&tv[0], 0, &atime);
	int hasMtime = getRequestedTime(&tv[1], 0, &mtime);
	if (hasAtime < 0) {
		return hasAtime;
	}
	if (hasMtime < 0) {
		return hasMtime;
	}
	if (hasAtime != 0 && hasMtime != 0) {
		// both UTIME_OMIT
		return 0;
	}

	if (file != NULL && file->dirtyFlags != DirtyFlagNotDirty) {
		if (flushFile(file) != 0) {
			return -EIO;
		}
	}

	// another client may have removed the directory (or its parents);
	// that is written first, the setTimes after it
	int result = recreatePendingDirs(file != NULL ? file->parentDir : dir);
	if (result != 0) {
		return result;
	}

	// UTIME_NOW is the time of the action (as for an edit); the times that
	// aren't given are kept
	int64_t actionTime = getNewActionTime();
	int64_t now = microsecondsToNanoseconds(actionTime);
	hasAtime = getRequestedTime(&tv[0], now, &atime) == 0;
	hasMtime = getRequestedTime(&tv[1], now, &mtime) == 0;

	Action* newAction = calloc(1, sizeof(Action));
	if (newAction == NULL) {
		logPrintf(LOG_ERROR, "bucse_utimens: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = actionTime;
	newAction->actionType = ActionTypeSetTimes;
	newAction->path = strdup(path+1);
	if (newAction->path == NULL) {
		logPrintf(LOG_ERROR, "bucse_utimens: strdup(): %s\n", strerror(errno));
		free(newAction);
		return -ENOMEM;
	}
	newAction->hasAtime = hasAtime;
	newAction->atime = hasAtime ? atime : 0;
	newAction->hasMtime = hasMtime;
	newAction->mtime = hasMtime ? mtime : 0;

	if (encryptAndAddActionFile(newAction) != 0) {
		logPrintf(LOG_ERROR, "bucse_utimens: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		return -EIO;
	}

	addAction(newAction);

	int64_t* targetAtime = file != NULL ? &file->atime : &dir->atime;
	int64_t* targetMtime = file != NULL ? &file->mtime : &dir->mtime;
	if (hasAtime) {
		*targetAtime = atime;
	}
	if (hasMtime) {
		*targetMtime = mtime;
	}

	return 0;
}

int bucse_utimens_guarded(const char *path, const struct timespec tv[2],
	struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_utimens(path, tv, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

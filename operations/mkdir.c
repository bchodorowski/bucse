#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fuse.h>
#include <pthread.h>

#include "../dynarray.h"
#include "../filesystem.h"
#include "../actions.h"
#include "../time.h"
#include "../log.h"
#include "../conf.h"

#include "operations.h"

#include "mkdir.h"

int bucse_mkdir(const char *path, mode_t mode)
{
	(void) mode;

	return makeDirectory(path, 0, 0);
}

int makeDirectory(const char *path, int hasMtime, int64_t mtime)
{
	logPrintf(LOG_DEBUG, "mkdir %s\n", path);

	if (confIsReadOnly()) {
		logPrintf(LOG_ERROR, "bucse_mkdir: cannot do that in readOnly mode\n");
		return -EROFS;
	}

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *containingDir = NULL;
	const char *dirName = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		dirName = path_split(path+1, &pathArray);
		if (dirName == NULL) {
			logPrintf(LOG_ERROR, "bucse_mkdir: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_mkdir: path not found when adding directory %s\n", path);
			return -ENOENT;
		}

		FilesystemDir *dir = findDir(containingDir, dirName);
		if (dir != NULL) {
			return -EEXIST;
		}
		FilesystemFile* file = findFile(containingDir, dirName);
		if (file != NULL) {
			return -EEXIST;
		}
	} else {
		return -ENOENT;
	}

	// another client may have removed the containing directory
	int result = recreatePendingDirs(containingDir);
	if (result != 0) {
		return result;
	}

	// construct new action, add it to actions
	Action* newAction = calloc(1, sizeof(Action));
	if (newAction == NULL) {
		logPrintf(LOG_ERROR, "bucse_mkdir: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getNewActionTime();
	newAction->actionType = ActionTypeAddDirectory;

	newAction->path = strdup(path+1);
	if (newAction->path == NULL) {
		logPrintf(LOG_ERROR, "bucse_mkdir: strdup() failed: %s\n", strerror(errno));
		free(newAction);
		return -ENOMEM;
	}
	newAction->content = NULL;
	newAction->contentLen = 0;
	newAction->size = 0;
	newAction->blockSize = 0;

	FilesystemDir* newDir = malloc(sizeof(FilesystemDir));
	if (newDir == NULL) {
		logPrintf(LOG_ERROR, "bucse_mkdir: malloc(): %s\n", strerror(errno));
		free(newAction->path);
		free(newAction);
		return -ENOMEM;
	}
	memset(newDir, 0, sizeof(FilesystemDir));

	// the name is owned by the action
	newDir->name = path_lastPart(newAction->path);

	if (hasMtime) {
		newAction->hasMtime = 1;
		newAction->mtime = mtime;
	}
	newDir->atime = microsecondsToNanoseconds(newAction->time);
	newDir->mtime = hasMtime ? mtime : newDir->atime;
	newDir->parentDir = containingDir;

	// once the action file is written, adding the directory can't fail
	if (reserveDynArray(&containingDir->dirs, 1) != 0) {
		free(newAction->path);
		free(newAction);
		free(newDir);
		return -ENOMEM;
	}

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		logPrintf(LOG_ERROR, "bucse_mkdir: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		free(newDir);
		return -EIO;
	}

	// add to actions array
	addAction(newAction);

	// update filesystem (room reserved above)
	addToDynArray(&containingDir->dirs, newDir);

	return 0;
}

int bucse_mkdir_guarded(const char *path, mode_t mode)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_mkdir(path, mode);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


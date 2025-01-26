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

#include "operations.h"

#include "mkdir.h"

static int bucse_mkdir(const char *path, mode_t mode)
{
	(void) mode;

	logPrintf(LOG_DEBUG, "mkdir %s\n", path);

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

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		logPrintf(LOG_ERROR, "bucse_mkdir: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getCurrentTime();
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

	// funny way to find the const char* filename that's owned by the action
	DynArray pathArray;
	memset(&pathArray, 0, sizeof(DynArray));
	newDir->name = path_split(newAction->path, &pathArray);
	path_free(&pathArray);

	newDir->atime = newDir->mtime = newAction->time;
	newDir->parentDir = containingDir;

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

	// update filesystem
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


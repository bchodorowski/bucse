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

#include "rmdir.h"

int bucse_rmdir(const char *path)
{
	logPrintf(LOG_DEBUG, "rmdir %s\n", path);

	if (confIsReadOnly()) {
		logPrintf(LOG_ERROR, "bucse_rmdir: cannot do that in readOnly mode\n");
		return -EROFS;
	}

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *containingDir = NULL;
	const char *dirName = NULL;
	FilesystemDir *dir = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		dirName = path_split(path+1, &pathArray);
		if (dirName == NULL) {
			logPrintf(LOG_ERROR, "bucse_rmdir: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_rmdir: path not found when adding directory %s\n", path);
			return -ENOENT;
		}

		dir = findDir(containingDir, dirName);
		if (dir == NULL) {
			FilesystemFile* file = findFile(containingDir, dirName);
			if (file != NULL) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the dir below
	} else {
		return -ENOENT;
	}

	if (dir->files.len > 0 || dir->dirs.len > 0) {
		logPrintf(LOG_ERROR, "bucse_rmdir: can't delete directory %s because it is not empty\n", path+1);
		return -ENOTEMPTY;
	}

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		logPrintf(LOG_ERROR, "bucse_rmdir: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getCurrentTime();
	newAction->actionType = ActionTypeRemoveDirectory;

	newAction->path = strdup(path+1);
	if (newAction->path == NULL) {
		logPrintf(LOG_ERROR, "bucse_rmdir: strdup() failed: %s\n", strerror(errno));
		free(newAction);
		return -ENOMEM;
	}
	newAction->content = NULL;
	newAction->contentLen = 0;
	newAction->size = 0;
	newAction->blockSize = 0;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		logPrintf(LOG_ERROR, "bucse_rmdir: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		return -EIO;
	}

	// add to actions array
	addAction(newAction);

	// update filesystem
	if (removeFromDynArrayUnordered(&containingDir->dirs, (void*)dir) != 0) {
		logPrintf(LOG_ERROR, "bucse_rmdir: removeFromDynArrayUnordered() failed\n");
		return -EIO;
	}
	freeDynArray(&dir->dirs);
	freeDynArray(&dir->files);
	free(dir);

	return 0;
}

int bucse_rmdir_guarded(const char *path)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_rmdir(path);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}



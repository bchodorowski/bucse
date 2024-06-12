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

#include "operations.h"

#include "unlink.h"

static int bucse_unlink(const char *path)
{
	fprintf(stderr, "DEBUG: unlink %s\n", path);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;
	FilesystemDir *containingDir = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_unlink: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_unlink: path not found when deleting file %s\n", path);
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

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		fprintf(stderr, "bucse_unlink: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getCurrentTime();
	newAction->actionType = ActionTypeRemoveFile;

	newAction->path = getFullFilePath(file);
	if (newAction->path == NULL) {
		fprintf(stderr, "bucse_unlink: getFullFilePath() failed: %s\n", strerror(errno));
		free(newAction);
		return -ENOMEM;
	}
	newAction->content = NULL;
	newAction->contentLen = 0;
	newAction->size = 0;
	newAction->blockSize = 0;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		fprintf(stderr, "bucse_unlink: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		return -EIO;
	}

	// add to actions array
	addAction(newAction);

	// update filesystem
	if (removeFromDynArrayUnordered(&containingDir->files, (void*)file) != 0) {
		fprintf(stderr, "bucse_unlink: removeFromDynArrayUnordered() failed\n");
		return -EIO;
	}
	free(file);

	return 0;
}

int bucse_unlink_guarded(const char *path)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_unlink(path);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


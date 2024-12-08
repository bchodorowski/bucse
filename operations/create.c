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
#include "open.h"

#include "create.h"

static int bucse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	fprintf(stderr, "DEBUG: create %s, mode %d, access mode %d\n", path, mode, fi->flags);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *containingDir = NULL;
	const char *fileName = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_create: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);
		if (containingDir == NULL) {
			fprintf(stderr, "bucse_create: path not found when creating file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			if ((fi->flags & O_CREAT) && (fi->flags & O_EXCL)) {
				return -EEXIST;
			}
			return bucse_open_guarded(path, fi);
		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EEXIST;
			} else {
				// continue below
			}
		}
	} else {
		return -ENOENT;
	}

	FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
	if (newFile == NULL) {
		fprintf(stderr, "bucse_create: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}

	// strdup here, will set DirtyFlagPendingCreate
	// and clean up when flushing the file in flush.c
	newFile->name = strdup(fileName);
	if (newFile->name == NULL) {
		fprintf(stderr, "splitPath: strdup(): %s\n", strerror(errno));
		free(newFile);
		return -ENOMEM;
	}
	newFile->atime = newFile->mtime = getCurrentTime();
	newFile->content = NULL;
	newFile->contentLen = 0;
	newFile->size = 0;
	newFile->blockSize = 0;
	newFile->dirtyFlags = DirtyFlagPendingCreate;
	memset(&newFile->pendingWrites, 0, sizeof(DynArray));
	newFile->parentDir = containingDir;

	addToDynArray(&containingDir->files, newFile);
	return 0;
}

int bucse_create_guarded(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_create(path, mode, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


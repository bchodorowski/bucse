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

#include "open.h"

static int bucse_open(const char *path, struct fuse_file_info *fi)
{
	logPrintf(LOG_DEBUG, "open %s, access mode %d\n", path, fi->flags);

	if (path == NULL) {
		return -EIO;
	}

	if (confIsReadOnly() && (fi->flags & (O_WRONLY | O_RDWR))) {
		logPrintf(LOG_ERROR, "bucse_open: cannot do that in readOnly mode\n");
		return -EROFS;
	}

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_open: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_open: path not found when opening file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			if ((fi->flags & O_CREAT) && (fi->flags & O_EXCL)) {
				return -EEXIST;
			}

			if (fi->flags & O_TRUNC) {
				file->truncSize = 0;
				file->dirtyFlags |= DirtyFlagPendingTrunc;
			}

			return 0;
		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				if (fi->flags & O_CREAT) {
					FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
					if (newFile == NULL) {
						logPrintf(LOG_ERROR, "bucse_open: malloc(): %s\n", strerror(errno));
						return -ENOMEM;
					}

					// strdup here, will set DirtyFlagPendingCreate
					// and clean up when flushing the file in flush.c
					newFile->name = strdup(fileName);
					if (newFile->name == NULL) {
						logPrintf(LOG_ERROR, "bucse_open: strdup(): %s\n", strerror(errno));
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
				return -ENOENT;
			}
		}
	} else {
		return -ENOENT;
	}
}

int bucse_open_guarded(const char *path, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_open(path, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


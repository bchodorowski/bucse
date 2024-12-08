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

#include "rename.h"


#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE	(1 << 0)	/* Don't overwrite target */
#endif

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE		(1 << 1)	/* Exchange source and dest */
#endif

static int bucse_rename(const char *srcPath, const char *dstPath,
		unsigned int flags)
{
	fprintf(stderr, "DEBUG: rename %s %s\n", srcPath, dstPath);

	if (srcPath == NULL || dstPath == NULL) {
		return -EIO;
	}

	FilesystemFile *srcFile = NULL;
	FilesystemDir *srcContainingDir = NULL;

	if (strcmp(srcPath, "/") == 0) {
		return -EACCES;
	} else if (srcPath[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(srcPath+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_rename: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		srcContainingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (srcContainingDir == NULL) {
			fprintf(stderr, "bucse_rename: path not found when moving file %s\n", srcPath);
			return -ENOENT;
		}

		srcFile = findFile(srcContainingDir, fileName);
		if (srcFile == NULL) {
			FilesystemDir* dir = findDir(srcContainingDir, fileName);
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

	FilesystemFile *dstFile = NULL;
	FilesystemDir *dstContainingDir = NULL;
	const char *dstFileName;

	if (strcmp(dstPath, "/") == 0) {
		return -EACCES;
	} else if (dstPath[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(dstPath+1, &pathArray);
		dstFileName = fileName;
		if (fileName == NULL) {
			fprintf(stderr, "bucse_rename: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		dstContainingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (dstContainingDir == NULL) {
			fprintf(stderr, "bucse_rename: path not found when moving file %s\n", dstPath);
			return -ENOENT;
		}

		dstFile = findFile(dstContainingDir, fileName);

		if (flags & RENAME_NOREPLACE) {
			if (dstFile != NULL) {
				fprintf(stderr, "bucse_rename: noreplace but destination file %s found\n", dstPath);
				return -EEXIST;
			}
			FilesystemDir* dir = findDir(dstContainingDir, fileName);
			if (dir) {
				fprintf(stderr, "bucse_rename: noreplace but destination %s is a directory\n", dstPath);
				return -EISDIR;
			}

		} else if (flags & RENAME_EXCHANGE) {
			if (dstFile == NULL) {
				FilesystemDir* dir = findDir(dstContainingDir, fileName);
				if (dir) {
					fprintf(stderr, "bucse_rename: exchange but destination %s is a directory\n", dstPath);
					return -EACCES;
				} else {
					fprintf(stderr, "bucse_rename: exchange but destination file %s not found\n", dstPath);
					return -ENOENT;
				}
			}

		} else {
			if (dstFile == NULL) {
				FilesystemDir* dir = findDir(dstContainingDir, fileName);
				if (dir) {
					fprintf(stderr, "bucse_rename: destination %s is a directory\n", dstPath);
					return -EISDIR;
				}
			}
		}

		// continue working with the file below
	} else {
		return -ENOENT;
	}

	// construct new action for destination, add it to actions
	Action* newDstAction = malloc(sizeof(Action));
	if (newDstAction == NULL) {
		fprintf(stderr, "bucse_rename: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newDstAction->time = getCurrentTime();
	newDstAction->actionType = dstFile ? ActionTypeEditFile : ActionTypeAddFile;
	newDstAction->path = strdup(dstPath+1);

	if (newDstAction->path == NULL) {
		fprintf(stderr, "bucse_rename: strdup(): %s\n", strerror(errno));
		free(newDstAction);
		return -ENOMEM;
	}
	newDstAction->content = malloc(srcFile->contentLen * MAX_STORAGE_NAME_LEN);
	if (newDstAction->content == NULL) {
		fprintf(stderr, "bucse_rename: malloc(): %s\n", strerror(errno));
		free(newDstAction->path);
		free(newDstAction);
		return -ENOMEM;
	}
	memcpy(newDstAction->content, srcFile->content, srcFile->contentLen * MAX_STORAGE_NAME_LEN);

	newDstAction->contentLen = srcFile->contentLen;
	newDstAction->size = srcFile->size;
	newDstAction->blockSize = srcFile->blockSize;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newDstAction) != 0) {
		fprintf(stderr, "bucse_rename: encryptAndAddActionFile failed\n");
		free(newDstAction->content);
		free(newDstAction->path);
		free(newDstAction);
		return -EIO;
	}

	// add to actions array
	addAction(newDstAction);

	// construct new action for source, add it to actions
	Action* newSrcAction = malloc(sizeof(Action));
	if (newSrcAction == NULL) {
		fprintf(stderr, "bucse_rename: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newSrcAction->time = newDstAction->time;
	newSrcAction->actionType = ActionTypeRemoveFile;

	newSrcAction->path = getFullFilePath(srcFile);
	if (newSrcAction->path == NULL) {
		fprintf(stderr, "bucse_rename: getFullFilePath() failed: %s\n", strerror(errno));
		free(newSrcAction);
		return -ENOMEM;
	}
	newSrcAction->content = NULL;
	newSrcAction->contentLen = 0;
	newSrcAction->size = 0;
	newSrcAction->blockSize = 0;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newSrcAction) != 0) {
		fprintf(stderr, "bucse_rename: encryptAndAddActionFile failed\n");
		free(newSrcAction->path);
		free(newSrcAction);
		return -EIO;
	}

	// add to actions array
	addAction(newSrcAction);

	// update filesystem
	if (dstFile == NULL) {
		dstFile = srcFile;
	} else {
		// move file from src to dst
		dstFile->content = newDstAction->content;
		dstFile->contentLen = newDstAction->contentLen;
		dstFile->size = newDstAction->size;
		dstFile->blockSize = newDstAction->blockSize;
		//dstFile->content = srcFile->content;
		//dstFile->contentLen = srcFile->contentLen;
		//dstFile->size = srcFile->size;
		//dstFile->blockSize = srcFile->blockSize;

		if (removeFromDynArrayUnordered(&srcContainingDir->files, (void*)srcFile) != 0) {
			fprintf(stderr, "bucse_unlink: removeFromDynArrayUnordered() failed\n");
			return -EIO;
		}
		free(srcFile);
	}

	dstFile->atime = dstFile->mtime = newDstAction->time;
	dstFile->parentDir = dstContainingDir;

	// get pointer to file name from newDstAction->path
	DynArray pathArray;
	memset(&pathArray, 0, sizeof(DynArray));
	const char *fileName = path_split(newDstAction->path, &pathArray);
	if (fileName == NULL) {
		fprintf(stderr, "doAction: path_split() failed\n");
		return -EIO;
	}
	path_free(&pathArray);

	dstFile->name = fileName;

	return 0;
}
	
int bucse_rename_guarded(const char *srcPath, const char *dstPath,
		unsigned int flags)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_rename(srcPath, dstPath, flags);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


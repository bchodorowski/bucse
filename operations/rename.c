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
#include "rmdir.h"
#include "flush.h"

#include "rename.h"


#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE	(1 << 0)	/* Don't overwrite target */
#endif

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE		(1 << 1)	/* Exchange source and dest */
#endif

static int renameFile(FilesystemFile *srcFile, FilesystemDir *srcContainingDir,
	FilesystemFile *dstFile, FilesystemDir *dstContainingDir,
	const char* dstPath)
{
	// flush srcFile to be sure pending writes have been saved
	if (srcFile->dirtyFlags != DirtyFlagNotDirty) {
		if (flushFile(srcFile) != 0) {
			return -EIO;
		}
	}

	// another client may have removed the destination directory
	int result = recreatePendingDirs(dstContainingDir);
	if (result != 0) {
		return result;
	}

	// construct new action for destination, add it to actions
	Action* newDstAction = calloc(1, sizeof(Action));
	if (newDstAction == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newDstAction->time = getNewActionTime();
	newDstAction->actionType = dstFile ? ActionTypeEditFile : ActionTypeAddFile;
	newDstAction->path = strdup(dstPath+1);

	if (newDstAction->path == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: strdup(): %s\n", strerror(errno));
		free(newDstAction);
		return -ENOMEM;
	}
	newDstAction->content = malloc(srcFile->contentLen * MAX_STORAGE_NAME_LEN);
	if (newDstAction->content == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
		free(newDstAction->path);
		free(newDstAction);
		return -ENOMEM;
	}
	memcpy(newDstAction->content, srcFile->content, srcFile->contentLen * MAX_STORAGE_NAME_LEN);

	newDstAction->contentLen = srcFile->contentLen;
	newDstAction->size = srcFile->size;
	newDstAction->blockSize = srcFile->blockSize;
	// a renamed file keeps its mtime (and atime, which isn't saved)
	newDstAction->hasMtime = 1;
	newDstAction->mtime = srcFile->mtime;
	int64_t atime = srcFile->atime;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newDstAction) != 0) {
		logPrintf(LOG_ERROR, "bucse_rename: encryptAndAddActionFile failed\n");
		free(newDstAction->content);
		free(newDstAction->path);
		free(newDstAction);
		return -EIO;
	}

	// add to actions array
	addAction(newDstAction);

	// construct new action for source, add it to actions
	Action* newSrcAction = calloc(1, sizeof(Action));
	if (newSrcAction == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newSrcAction->time = newDstAction->time;
	newSrcAction->actionType = ActionTypeRemoveFile;

	newSrcAction->path = getFullFilePath(srcFile);
	if (newSrcAction->path == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: getFullFilePath() failed: %s\n", strerror(errno));
		free(newSrcAction);
		return -ENOMEM;
	}
	newSrcAction->content = NULL;
	newSrcAction->contentLen = 0;
	newSrcAction->size = 0;
	newSrcAction->blockSize = 0;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newSrcAction) != 0) {
		logPrintf(LOG_ERROR, "bucse_rename: encryptAndAddActionFile failed\n");
		free(newSrcAction->path);
		free(newSrcAction);
		return -EIO;
	}

	// add to actions array
	addAction(newSrcAction);

	// update filesystem
	if (removeFromDynArrayUnordered(&srcContainingDir->files, (void*)srcFile) != 0) {
		logPrintf(LOG_ERROR, "bucse_rename: removeFromDynArrayUnordered() failed\n");
		return -EIO;
	}

	if (dstFile == NULL) {
		dstFile = srcFile;
		addToDynArray(&dstContainingDir->files, dstFile);
	} else {
		// move file from src to dst
		dstFile->content = newDstAction->content;
		dstFile->contentLen = newDstAction->contentLen;
		dstFile->size = newDstAction->size;
		dstFile->blockSize = newDstAction->blockSize;

		free(srcFile);
	}

	dstFile->atime = atime;
	dstFile->mtime = newDstAction->mtime;
	dstFile->parentDir = dstContainingDir;

	// get pointer to file name from newDstAction->path
	DynArray pathArray;
	memset(&pathArray, 0, sizeof(DynArray));
	const char *fileName = path_split(newDstAction->path, &pathArray);
	if (fileName == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: path_split() failed\n");
		return -EIO;
	}
	path_free(&pathArray);

	dstFile->name = fileName;

	return 0;
}

// Returns a new editFile action for file's path that gives it the content
// (and mtime) of other, or NULL.
static Action* newExchangeAction(FilesystemFile *file, FilesystemFile *other, int64_t time)
{
	Action* action = calloc(1, sizeof(Action));
	if (action == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
		return NULL;
	}
	action->time = time;
	action->actionType = ActionTypeEditFile;
	action->path = getFullFilePath(file);
	// malloc(0) may return NULL, so allocate at least one slot
	action->content = malloc((other->contentLen > 0 ? other->contentLen : 1) * MAX_STORAGE_NAME_LEN);
	if (action->path == NULL || action->content == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
		free(action->path);
		free(action->content);
		free(action);
		return NULL;
	}
	memcpy(action->content, other->content, other->contentLen * MAX_STORAGE_NAME_LEN);
	action->contentLen = other->contentLen;
	action->size = other->size;
	action->blockSize = other->blockSize;
	// the exchanged files keep their mtimes
	action->hasMtime = 1;
	action->mtime = other->mtime;
	return action;
}

// RENAME_EXCHANGE of two files: an editFile for each path, with the content
// of the other one. Both are written in one action file, so no client (and
// no crash) ever sees only one of them applied.
static int exchangeFiles(FilesystemFile *srcFile, FilesystemDir *srcContainingDir,
	FilesystemFile *dstFile, FilesystemDir *dstContainingDir)
{
	if (srcFile == dstFile) {
		return 0;
	}

	// flush both files to be sure pending writes have been saved
	if (srcFile->dirtyFlags != DirtyFlagNotDirty && flushFile(srcFile) != 0) {
		return -EIO;
	}
	if (dstFile->dirtyFlags != DirtyFlagNotDirty && flushFile(dstFile) != 0) {
		return -EIO;
	}

	// another client may have removed a containing directory
	int result = recreatePendingDirs(srcContainingDir);
	if (result != 0) {
		return result;
	}
	result = recreatePendingDirs(dstContainingDir);
	if (result != 0) {
		return result;
	}

	int64_t time = getNewActionTime();
	Action* newActions[2];
	newActions[0] = newExchangeAction(srcFile, dstFile, time);
	newActions[1] = newExchangeAction(dstFile, srcFile, time);
	if (newActions[0] == NULL || newActions[1] == NULL) {
		for (int i=0; i<2; i++) {
			if (newActions[i] != NULL) {
				free(newActions[i]->content);
				free(newActions[i]->path);
				free(newActions[i]);
			}
		}
		return -ENOMEM;
	}

	if (encryptAndAddActionFiles(newActions, 2) != 0) {
		logPrintf(LOG_ERROR, "bucse_rename: encryptAndAddActionFiles failed\n");
		for (int i=0; i<2; i++) {
			free(newActions[i]->content);
			free(newActions[i]->path);
			free(newActions[i]);
		}
		return -EIO;
	}

	// both before the tree changes, see addAction()
	addAction(newActions[0]);
	addAction(newActions[1]);

	// update filesystem; the atime isn't saved, it is swapped in memory
	FilesystemFile* files[2] = { srcFile, dstFile };
	int64_t atimes[2] = { dstFile->atime, srcFile->atime };
	for (int i=0; i<2; i++) {
		files[i]->content = newActions[i]->content;
		files[i]->contentLen = newActions[i]->contentLen;
		files[i]->size = newActions[i]->size;
		files[i]->blockSize = newActions[i]->blockSize;
		files[i]->mtime = newActions[i]->mtime;
		files[i]->atime = atimes[i];
	}

	return 0;
}

static int renameDir(FilesystemDir *srcDir, FilesystemDir *srcContainingDir,
	FilesystemDir *dstDir, FilesystemDir *dstContainingDir,
	const char* dstPath)
{
	if (dstDir && (dstDir->files.len > 0 || dstDir->dirs.len > 0)) {
		logPrintf(LOG_ERROR, "bucse_rename: dir not empty: %s\n", dstPath+1);
		return -ENOTEMPTY;
	}

	if (dstDir == srcDir) {
		return 0;
	}

	int result;

	if (dstDir) {
		// an existing empty directory is replaced: reuse it; if another
		// client removed it, add it again
		result = recreatePendingDirs(dstDir);
	} else {
		// create the target directory
		// it keeps the source directory's mtime
		result = makeDirectory(dstPath, 1, srcDir->mtime);
	}

	if (result != 0) {
		logPrintf(LOG_ERROR, "bucse_rename: creating the destination directory failed\n");
		return result;
	}

	// find dstDir
	if (dstDir == NULL) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *dirName = path_split(dstPath+1, &pathArray);
		path_free(&pathArray);
		if (dirName == NULL) {
			logPrintf(LOG_ERROR, "bucse_rename: path_split() failed\n");
			return -ENOMEM;
		}

		dstDir = findDir(dstContainingDir, dirName);
	}

	if (dstDir == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: No destination directory found after bucse_mkdir() succeeded\n");
		return -EIO;
	}

	// recursively move everything from the directory
	while (srcDir->files.len > 0) {
		FilesystemFile *f = srcDir->files.objects[0];
		size_t dstPathLen = strlen(dstPath);
		size_t fNameLen = strlen(f->name);

		char* newDstPath = malloc(dstPathLen + 2 + fNameLen);
		if (newDstPath == NULL) {
			logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
			return -ENOMEM;
		}
		memcpy(newDstPath, dstPath, dstPathLen);
		newDstPath[dstPathLen] = '/';
		memcpy(newDstPath+dstPathLen+1, f->name, fNameLen+1);

		result = renameFile(f, srcDir, NULL, dstDir, newDstPath);
		free(newDstPath);

		if (result != 0) {
			logPrintf(LOG_ERROR, "bucse_rename: renameFile() failed\n");
			return result;
		}
	}
	while (srcDir->dirs.len > 0) {
		FilesystemDir *d = srcDir->dirs.objects[0];
		size_t dstPathLen = strlen(dstPath);
		size_t dNameLen = strlen(d->name);

		char* newDstPath = malloc(dstPathLen + 2 + dNameLen);
		if (newDstPath == NULL) {
			logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
			return -ENOMEM;
		}
		memcpy(newDstPath, dstPath, dstPathLen);
		newDstPath[dstPathLen] = '/';
		memcpy(newDstPath+dstPathLen+1, d->name, dNameLen+1);

		result = renameDir(d, srcDir, NULL, dstDir, newDstPath);
		free(newDstPath);

		if (result != 0) {
			logPrintf(LOG_ERROR, "bucse_rename: renameFile() failed\n");
			return result;
		}
	}

	// find srcPath
	char* srcPathWithoutFirstSlash = getFullDirPath(srcDir);
	if (srcPathWithoutFirstSlash == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: getFullFilePath() failed: %s\n", strerror(errno));
		return -ENOMEM;
	}

	char* srcPath = malloc(strlen(srcPathWithoutFirstSlash) + 2);
	if (srcPath == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
		free(srcPathWithoutFirstSlash);
		return -ENOMEM;
	}
	srcPath[0] = '/';
	memcpy(srcPath+1, srcPathWithoutFirstSlash, strlen(srcPathWithoutFirstSlash)+1);
	free(srcPathWithoutFirstSlash);

	// remove the source directory
	result = bucse_rmdir(srcPath);
	free(srcPath);

	if (result != 0) {
		logPrintf(LOG_ERROR, "bucse_rename: bucse_rmdir() failed\n");
		return result;
	}
	return 0;
}

static int bucse_rename(const char *srcPath, const char *dstPath,
		unsigned int flags)
{
	logPrintf(LOG_DEBUG, "rename %s %s\n", srcPath, dstPath);

	if (confIsReadOnly()) {
		logPrintf(LOG_ERROR, "bucse_rename: cannot do that in readOnly mode\n");
		return -EROFS;
	}

	if (srcPath == NULL || dstPath == NULL) {
		return -EIO;
	}

	if ((flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) != 0
		|| ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE))) {
		logPrintf(LOG_ERROR, "bucse_rename: unsupported flags %u\n", flags);
		return -EINVAL;
	}

	FilesystemFile *srcFile = NULL;
	FilesystemDir *srcDir = NULL;
	FilesystemDir *srcContainingDir = NULL;

	if (strcmp(srcPath, "/") == 0) {
		return -EACCES;
	} else if (srcPath[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(srcPath+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_rename: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		srcContainingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (srcContainingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_rename: path not found when moving file %s\n", srcPath);
			return -ENOENT;
		}

		srcFile = findFile(srcContainingDir, fileName);
		if (srcFile == NULL) {
			FilesystemDir* dir = findDir(srcContainingDir, fileName);
			if (dir) {
				srcDir = dir;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	// srcContainingDir is not NULL
	// either srcFile or srcDir is not NULL

	FilesystemFile *dstFile = NULL;
	FilesystemDir *dstDir = NULL;
	FilesystemDir *dstContainingDir = NULL;

	if (strcmp(dstPath, "/") == 0) {
		return -EACCES;
	} else if (dstPath[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(dstPath+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_rename: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		dstContainingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (dstContainingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_rename: path not found when moving file %s\n", dstPath);
			return -ENOENT;
		}

		dstFile = findFile(dstContainingDir, fileName);
		dstDir = findDir(dstContainingDir, fileName);

		if (flags & RENAME_EXCHANGE) {
			if (dstFile == NULL && dstDir == NULL) {
				logPrintf(LOG_ERROR, "bucse_rename: exchange but destination %s not found\n", dstPath);
				return -ENOENT;
			}
			// swapping directories would mean rewriting everything
			// in them, see "Non-goals" in docs/design.md
			if (srcDir != NULL || dstDir != NULL) {
				logPrintf(LOG_ERROR, "bucse_rename: exchange of directories is not supported: %s %s\n", srcPath, dstPath);
				return -EINVAL;
			}
			return exchangeFiles(srcFile, srcContainingDir, dstFile, dstContainingDir);
		}

		if (srcFile != NULL) {
			// rename file

			if (dstDir) {
				logPrintf(LOG_ERROR, "bucse_rename: destination %s is a directory\n", dstPath);
				return -EISDIR;
			}

			if (flags & RENAME_NOREPLACE) {
				if (dstFile != NULL) {
					logPrintf(LOG_ERROR, "bucse_rename: noreplace but destination file %s found\n", dstPath);
					return -EEXIST;
				}

			}

		} else if (srcDir != NULL) {
			if (dstFile) {
				logPrintf(LOG_ERROR, "bucse_rename: destination %s is not a directory\n", dstPath);
				return -ENOTDIR;
			}
			if (flags & RENAME_NOREPLACE) {
				if (dstDir != NULL) {
					logPrintf(LOG_ERROR, "bucse_rename: noreplace but destination dir %s found\n", dstPath);
					return -EEXIST;
				}

			}
		} else {
			logPrintf(LOG_ERROR, "bucse_rename: Unexpected state\n");
			return -EIO;
		}

		// continue working with the file below
	} else {
		return -ENOENT;
	}

	if (srcFile != NULL) {
		return renameFile(srcFile, srcContainingDir, dstFile, dstContainingDir, dstPath);
	} else if (srcDir != NULL) {
		return renameDir(srcDir, srcContainingDir, dstDir, dstContainingDir, dstPath);
	} else {
		logPrintf(LOG_ERROR, "bucse_rename: Unexpected state\n");
		return -EIO;
	}

}
	
int bucse_rename_guarded(const char *srcPath, const char *dstPath,
		unsigned int flags)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_rename(srcPath, dstPath, flags);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


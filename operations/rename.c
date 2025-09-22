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

	// construct new action for destination, add it to actions
	Action* newDstAction = malloc(sizeof(Action));
	if (newDstAction == NULL) {
		logPrintf(LOG_ERROR, "bucse_rename: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newDstAction->time = getCurrentTime();
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
	Action* newSrcAction = malloc(sizeof(Action));
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

	dstFile->atime = dstFile->mtime = newDstAction->time;
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

static int renameDir(FilesystemDir *srcDir, FilesystemDir *srcContainingDir,
	FilesystemDir *dstDir, FilesystemDir *dstContainingDir,
	const char* dstPath)
{
	if (dstDir && (dstDir->files.len > 0 || dstDir->dirs.len > 0)) {
		logPrintf(LOG_ERROR, "bucse_rename: dir not empty: %s\n", dstPath+1);
		return -ENOTEMPTY;
	}

	int result;

	// create the target directory
	result = bucse_mkdir(dstPath, S_IRUSR | S_IWUSR | S_IXUSR
		| S_IRGRP | S_IXGRP
		| S_IROTH | S_IXOTH);

	if (result != 0) {
		logPrintf(LOG_ERROR, "bucse_rename: bucse_mkdir() failed\n");
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
		logPrintf(LOG_ERROR, "bucse_rename: No destination directory found after bucse_mkdir() suceeded\n");
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

	if (srcPath == NULL || dstPath == NULL) {
		return -EIO;
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

			} else if (flags & RENAME_EXCHANGE) {
				if (dstFile == NULL) {
					logPrintf(LOG_ERROR, "bucse_rename: exchange but destination file %s not found\n", dstPath);
					return -ENOENT;
				}
				// TODO: implement RENAME_EXCHANGE
				logPrintf(LOG_ERROR, "bucse_rename: RENAME_EXCHANGE not implemented\n");
				return -ENOSYS;
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

			} else if (flags & RENAME_EXCHANGE) {
				if (dstDir == NULL) {
					logPrintf(LOG_ERROR, "bucse_rename: exchange but destination dir %s not found\n", dstPath);
					return -ENOENT;
				}
				// TODO: implement RENAME_EXCHANGE
				logPrintf(LOG_ERROR, "bucse_rename: RENAME_EXCHANGE not implemented\n");
				return -ENOSYS;
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


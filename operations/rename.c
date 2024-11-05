#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fuse.h>
#include <pthread.h>

#include "../dynarray.h"
#include "../filesystem.h"
#include "../actions.h"

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

	if (strcmp(dstPath, "/") == 0) {
		return -EACCES;
	} else if (dstPath[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(dstPath+1, &pathArray);
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

	// TODO: actually move the file
	// return 0;
	return -ENOSYS;
}
	
int bucse_rename_guarded(const char *srcPath, const char *dstPath,
		unsigned int flags)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_rename(srcPath, dstPath, flags);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


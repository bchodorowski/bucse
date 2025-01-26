#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <errno.h>
#include <fuse.h>
#include <pthread.h>

#include "../dynarray.h"
#include "../filesystem.h"
#include "../actions.h"
#include "../log.h"

#include "operations.h"
#include "flush.h"
#include "getattr.h"

extern uid_t cachedUid;
extern gid_t cachedGid;

static struct timespec microsecondsToNanoseconds(int64_t t)
{
	struct timespec ts;
	ts.tv_sec = t / 1000000L;
	ts.tv_nsec = (t % 1000000L) * 1000L;
	return ts;
}

static int bucse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	(void) fi;

	if (path == NULL) {
		return -EIO;
	}

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;

		stbuf->st_atim = microsecondsToNanoseconds(root->atime);
		stbuf->st_mtim = microsecondsToNanoseconds(root->mtime);
		stbuf->st_ctim = microsecondsToNanoseconds(root->mtime);

		stbuf->st_uid = cachedUid;
		stbuf->st_gid = cachedGid;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_getattr: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			if (file->dirtyFlags & DirtyFlagPendingWrite) {
				flushFile(file);
			}

			stbuf->st_mode = S_IFREG | 0644;
			stbuf->st_nlink = 1;
			stbuf->st_size = (file->dirtyFlags & DirtyFlagPendingTrunc)
				? file->truncSize
				: file->size;
			//stbuf->st_ino = MurmurHash64(path, strlen(path), 0);
			stbuf->st_atim = microsecondsToNanoseconds(file->atime);
			stbuf->st_mtim = microsecondsToNanoseconds(file->mtime);
			stbuf->st_ctim = microsecondsToNanoseconds(file->mtime);

			stbuf->st_uid = cachedUid;
			stbuf->st_gid = cachedGid;

		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 1;
				//stbuf->st_ino = MurmurHash64(path, strlen(path), 0);
				stbuf->st_atim = microsecondsToNanoseconds(dir->atime);
				stbuf->st_mtim = microsecondsToNanoseconds(dir->mtime);
				stbuf->st_ctim = microsecondsToNanoseconds(dir->mtime);

				stbuf->st_uid = cachedUid;
				stbuf->st_gid = cachedGid;
			} else {
				return -ENOENT;
			}
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

int bucse_getattr_guarded(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_getattr(path, stbuf, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


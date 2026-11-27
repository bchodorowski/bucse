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
#include "../conf.h"

#include "operations.h"
#include "getattr.h"

extern uid_t cachedUid;
extern gid_t cachedGid;

static struct timespec toTimespec(int64_t t)
{
	struct timespec ts;
	ts.tv_sec = t / 1000000000L;
	ts.tv_nsec = t % 1000000000L;
	// a time before the epoch: tv_nsec must not be negative
	if (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000L;
	}
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
		if (confIsReadOnly()) {
			stbuf->st_mode = S_IFDIR | 0555;
		} else {
			stbuf->st_mode = S_IFDIR | 0755;
		}
		stbuf->st_nlink = 2;

		stbuf->st_atim = toTimespec(root->atime);
		stbuf->st_mtim = toTimespec(root->mtime);
		stbuf->st_ctim = toTimespec(root->mtime);

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
			// pending writes aren't flushed here: anything that stats a
			// file while it's being written (ls -l, a file manager, the
			// kernel itself) would flush it, and every flush rewrites the
			// partial block at the end. They are shown instead: the size
			// they extend the file to, and the time of the last one.
			size_t size = (file->dirtyFlags & DirtyFlagPendingTrunc)
				? file->truncSize
				: file->size;
			int64_t mtime = file->mtime;
			if (file->dirtyFlags & DirtyFlagPendingWrite) {
				for (int i = 0; i < file->pendingWrites.len; i++) {
					PendingWrite* pw = file->pendingWrites.objects[i];
					if (size < (size_t)pw->offset + pw->size) {
						size = (size_t)pw->offset + pw->size;
					}
				}
				mtime = file->pendingWriteMtime;
			}

			if (confIsReadOnly()) {
				//stbuf->st_mode = S_IFREG | 0444;
				stbuf->st_mode = S_IFREG | 0555;
			} else {
				//stbuf->st_mode = S_IFREG | 0644;
				stbuf->st_mode = S_IFREG | 0755;
			}
			stbuf->st_nlink = 1;
			stbuf->st_size = size;
			//stbuf->st_ino = MurmurHash64(path, strlen(path), 0);
			stbuf->st_atim = toTimespec(file->atime);
			stbuf->st_mtim = toTimespec(mtime);
			stbuf->st_ctim = toTimespec(mtime);

			stbuf->st_uid = cachedUid;
			stbuf->st_gid = cachedGid;

		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				if (confIsReadOnly()) {
					stbuf->st_mode = S_IFDIR | 0555;
				} else {
					stbuf->st_mode = S_IFDIR | 0755;
				}
				stbuf->st_nlink = 1;
				//stbuf->st_ino = MurmurHash64(path, strlen(path), 0);
				stbuf->st_atim = toTimespec(dir->atime);
				stbuf->st_mtim = toTimespec(dir->mtime);
				stbuf->st_ctim = toTimespec(dir->mtime);

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


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

#include "readdir.h"

static int bucse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	(void) offset;
	(void) fi;
	(void) flags;

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *dir;

	if (strcmp(path, "/") == 0) {
		dir = root;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_readdir: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		dir = findDirByPath(&pathArray);
		path_free(&pathArray);
	} else {
		return -ENOENT;
	}

	if (dir == NULL) {
		return -ENOENT;
	}

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	for (int i=0; i<dir->dirs.len; i++) {
		FilesystemDir* d = dir->dirs.objects[i];
		filler(buf, d->name, NULL, 0, 0);
	}

	for (int i=0; i<dir->files.len; i++) {
		FilesystemFile* f = dir->files.objects[i];
		filler(buf, f->name, NULL, 0, 0);
	}
	dir->atime = getCurrentTime();
	return 0;
}

int bucse_readdir_guarded(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_readdir(path, buf, filler, offset, fi, flags);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


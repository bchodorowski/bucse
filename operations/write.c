#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fuse.h>
#include <pthread.h>

#include "../dynarray.h"
#include "../filesystem.h"
#include "../actions.h"
#include "../log.h"
#include "../conf.h"
#include "../time.h"

#include "operations.h"
#include "flush.h"

#include "write.h"

// A file is flushed once its pending writes reach either limit, so that
// writing a big file (e.g. cp of a multi-gigabyte one) doesn't keep all of
// it in memory. The size limit is the maximum block size (flush.c): writes
// that go on from where the last flush ended then fill whole blocks, and
// fewer partial blocks are stored and then replaced. The count limit keeps
// the overhead of many small writes down; sequential writes are merged into
// one pending write (see below), so it's reached only by scattered ones.
#define MAX_PENDING_WRITES_SIZE (128 * 1024 * 1024)
#define MAX_PENDING_WRITES_COUNT 10240

static int bucse_write(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	logPrintf(LOG_DEBUG, "write %s, size: %zu, offset: %jd\n", path, size, (intmax_t)offset);

	if (confIsReadOnly()) {
		logPrintf(LOG_ERROR, "bucse_write: cannot do that in readOnly mode\n");
		return -EROFS;
	}

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "bucse_write: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_write: path not found when writing file %s\n", path);
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

	// a write that goes on right where the last pending write ends (e.g.
	// dd or cp writing a file sequentially) is appended to it, so that
	// small sequential writes don't reach MAX_PENDING_WRITES_COUNT: each
	// flush rewrites the partial block at the end, so flushing every few
	// MB would store many times the file's size
	PendingWrite* last = NULL;
	if (file->pendingWrites.len > 0) {
		last = file->pendingWrites.objects[file->pendingWrites.len - 1];
	}
	if (last != NULL && size > 0 && last->offset + (off_t)last->size == offset) {
		if (last->size + size > last->capacity) {
			size_t newCapacity = last->capacity * 2;
			if (newCapacity > MAX_PENDING_WRITES_SIZE) {
				newCapacity = MAX_PENDING_WRITES_SIZE;
			}
			if (newCapacity < last->size + size) {
				newCapacity = last->size + size;
			}
			char* newBuf = realloc(last->buf, newCapacity);
			if (newBuf == NULL) {
				logPrintf(LOG_ERROR, "bucse_write: realloc(): %s\n", strerror(errno));
				return -ENOMEM;
			}
			last->buf = newBuf;
			last->capacity = newCapacity;
		}
		memcpy(last->buf + last->size, buf, size);
		last->size += size;
	} else {
		PendingWrite* newPendingWrite = malloc(sizeof(PendingWrite));
		if (newPendingWrite == NULL) {
			logPrintf(LOG_ERROR, "bucse_write: malloc(): %s\n", strerror(errno));
			return -ENOMEM;
		}
		newPendingWrite->size = size;
		newPendingWrite->capacity = size;
		newPendingWrite->buf = malloc(size);
		if (newPendingWrite->buf == NULL) {
			logPrintf(LOG_ERROR, "bucse_write: malloc(): %s\n", strerror(errno));
			free(newPendingWrite);
			return -ENOMEM;
		}
		memcpy(newPendingWrite->buf, buf, size);
		newPendingWrite->offset = offset;
		if (addToDynArray(&file->pendingWrites, newPendingWrite) != 0) {
			free(newPendingWrite->buf);
			free(newPendingWrite);
			return -ENOMEM;
		}
	}
	file->pendingWritesSize += size;
	file->pendingWriteMtime = getCurrentTimeNs();
	file->dirtyFlags |= DirtyFlagPendingWrite;

	if (file->pendingWritesSize >= MAX_PENDING_WRITES_SIZE
		|| file->pendingWrites.len >= MAX_PENDING_WRITES_COUNT) {
		logPrintf(LOG_DEBUG, "bucse_write: too many pending writes (%d, %zu bytes), flushing\n",
			file->pendingWrites.len, file->pendingWritesSize);
		if (flushFile(file) != 0) {
			return -EIO;
		}
	}

	return size;
}

int bucse_write_guarded(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_write(path, buf, size, offset, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


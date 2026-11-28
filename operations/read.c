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
#include "flush.h"

#include "read.h"

typedef struct {
	const char* block;
	off_t offset;
	size_t len;
} BlockOffsetLen;

// use offset and size to determine which blocks contain the data that's needed
static int determineBlocksToRead(DynArray *blocksToRead, off_t offset, size_t size, FilesystemFile* file)
{
	if (offset > file->size) {
		return 0;
	}
	if (size > file->size - offset) {
		size = file->size - offset;
	}

	while (size > 0) {
		int blockIndex = offset / file->blockSize;
		off_t blockOffset = (offset % file->blockSize);
		size_t blockLen = file->blockSize - blockOffset;
		if (blockLen > size) {
			blockLen = size;
		}
		BlockOffsetLen *block = malloc(sizeof(BlockOffsetLen));
		if (!block) {
			logPrintf(LOG_ERROR, "determineBlocksToRead: malloc(): %s\n", strerror(errno));
			return 1;
		}
		block->block = file->content + MAX_STORAGE_NAME_LEN * blockIndex;
		block->offset = blockOffset;
		block->len = blockLen;
		if (addToDynArray(blocksToRead, block) != 0) {
			free(block);
			return 1;
		}

		offset += blockLen;
		size -= blockLen;
	}
	
	return 0;
}

static int bucse_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	logPrintf(LOG_DEBUG, "read %s, size: %zu, offset: %jd\n", path, size, (intmax_t)offset);

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
			logPrintf(LOG_ERROR, "bucse_read: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_read: path not found when reading file %s\n", path);
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

	if (file->dirtyFlags != DirtyFlagNotDirty) {
		if (flushFile(file) != 0) {
			return -EIO;
		}
	}

	// determine which blocks should be read
	DynArray blocksToRead;
	memset(&blocksToRead, 0, sizeof(DynArray));
	if (determineBlocksToRead(&blocksToRead, offset, size, file) != 0) {
		logPrintf(LOG_ERROR, "bucse_read: determineBlocksToRead failed\n");
		for (int i=0; i<blocksToRead.len; i++) {
			free(blocksToRead.objects[i]);
		}
		freeDynArray(&blocksToRead);
		return -ENOMEM;
	}

	// only the requested bytes are copied from the (cached) block: FUSE
	// reads in pieces of at most 128 KB, and a block can be up to 128 MB
	size_t copiedBytes = 0;
	int ioerror = 0;
	for (int i=0; i<blocksToRead.len; i++) {
		BlockOffsetLen* block = blocksToRead.objects[i];
		const char* blockData;
		size_t blockDataSize;

		if (decryptBlock(block->block, file->blockSize,
			block->offset + block->len,
			&blockData, &blockDataSize) != 0) {
			ioerror = 1;
			break;
		}

		memcpy(buf + copiedBytes, blockData + block->offset, block->len);
		copiedBytes += block->len;
	}

	for (int i=0; i<blocksToRead.len; i++) {
		free(blocksToRead.objects[i]);
	}
	freeDynArray(&blocksToRead);

	if (ioerror) {
		return -EIO;
	}

	file->atime = getCurrentTimeNs();
	return copiedBytes;
}

int bucse_read_guarded(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_read(path, buf, size, offset, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


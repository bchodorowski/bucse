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

#include "../destinations/dest.h"
#include "../encryption/encr.h"

#include "operations.h"
#include "flush.h"

#include "read.h"

extern Destination *destination;
extern Encryption *encryption;

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
		addToDynArray(blocksToRead, block);

		offset += blockLen;
		size -= blockLen;
	}
	
	return 0;
}

static int bucse_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	logPrintf(LOG_DEBUG, "read %s, size: %u, offset: %u\n", path, size, offset);

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
		flushFile(file);
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

	size_t maxEncryptedBlockSize = getMaxEncryptedBlockSize(file->blockSize);
	char* encryptedBlockBuf = malloc(maxEncryptedBlockSize);
	if (encryptedBlockBuf == NULL) {
		logPrintf(LOG_ERROR, "bucse_read: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}

	size_t maxDecryptedBlockSize = file->blockSize;
	char* decryptedBlockBuf = malloc(maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN);
	if (decryptedBlockBuf == NULL) {
		logPrintf(LOG_ERROR, "bucse_read: malloc(): %s\n", strerror(errno));
		free(encryptedBlockBuf);
		return -ENOMEM;
	}

	size_t copiedBytes = 0;
	int ioerror = 0;
	for (int i=0; i<blocksToRead.len; i++) {
		BlockOffsetLen* block = blocksToRead.objects[i];
		size_t encryptedBlockBufSize = maxEncryptedBlockSize;
		int res = destination->getStorageFile(block->block, encryptedBlockBuf, &encryptedBlockBufSize);
		if (res != 0) {
			logPrintf(LOG_ERROR, "bucse_read: getStorageFile failed for %s: %d\n",
				block->block, res);
			ioerror = 1;
			break;
		}

		size_t decryptedBlockBufSize = maxDecryptedBlockSize;
		res = encryption->decrypt(encryptedBlockBuf, encryptedBlockBufSize,
			decryptedBlockBuf, &decryptedBlockBufSize,
			conf.passphrase);
		if (res != 0) {
			logPrintf(LOG_ERROR, "bucse_read: decrypt failed: %d\n", res);
			ioerror = 1;
			break;
		}
		if (decryptedBlockBufSize < block->offset + block->len) {
			logPrintf(LOG_ERROR, "bucse_read: expected decrypted block size at least %d, got %d\n",
				block->offset + block->len, decryptedBlockBufSize);
			ioerror = 1;
			break;
		}

		memcpy(buf + copiedBytes, decryptedBlockBuf + block->offset, block->len);
		copiedBytes += block->len;
	}

	free(encryptedBlockBuf);
	free(decryptedBlockBuf);

	for (int i=0; i<blocksToRead.len; i++) {
		free(blocksToRead.objects[i]);
	}
	freeDynArray(&blocksToRead);

	if (ioerror) {
		return -EIO;
	}

	file->atime = getCurrentTime();
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


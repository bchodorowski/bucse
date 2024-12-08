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

#include "../conf.h"

#include "../destinations/dest.h"
#include "../encryption/encr.h"

#include "operations.h"

#include "flush.h"

#define MIN_BLOCK_SIZE 512
#define MAX_BLOCK_SIZE (128 * 1024 * 1024)

extern Destination *destination;
extern Encryption *encryption;

// determine block size using a file size
static int getBlockSize(int size)
{
	if (size == 0) {
		return 0;
	}

	// we want block size to be a power of 2,
	// in general, we aim to have between 4 and 8 blocks in a file,
	int result = 1;
	size /= 4;

	while (size) {
		size >>= 1;
		result <<= 1;
	}
	result >>= 1;

	// unless we hit those edge cases
	if (result < MIN_BLOCK_SIZE) {
		return MIN_BLOCK_SIZE;
	}

	if (result > MAX_BLOCK_SIZE) {
		return MAX_BLOCK_SIZE;
	}

	return result;
}

static int determineBlocksToWrite(char *blocksToWrite, off_t offset, size_t size, int fileBlockSize)
{
	while (size > 0) {
		int blockIndex = offset / fileBlockSize;
		off_t blockOffset = (offset % fileBlockSize);
		size_t blockLen = fileBlockSize - blockOffset;
		if (blockLen > size) {
			blockLen = size;
		}

		blocksToWrite[blockIndex] = 1;

		offset += blockLen;
		size -= blockLen;
	}
	
	return 0;
}

int flushFile(FilesystemFile* file)
{
	if (file->dirtyFlags == DirtyFlagNotDirty) {
		return 0;
	}
	// TODO: what if create only, no writes?

	int newSize = file->size;
	if (file->dirtyFlags & DirtyFlagPendingTrunc) {
		newSize = file->truncSize;
		file->truncSize = 0;
	}
	int newBlockSize = file->blockSize;
	int newContentLen = file->contentLen;
	char* newContent = NULL;

	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		if (newSize < (pw->offset + pw->size)) {
			newSize = (pw->offset + pw->size);
		}
	}

	// block size may not be determined yet if the file hasn't been flushed
	// with any data
	if (file->blockSize == 0) {
		newBlockSize = getBlockSize(newSize);
	}

	if (newBlockSize == 0) {
		newContentLen = 0;
	} else {
		newContentLen = newSize / newBlockSize + (int)(newSize % newBlockSize != 0);
	}

	// TODO: if newContentLen is big, consider changing the blockSize and
	//       rewriting the whole file

	if (file->pendingWrites.len == 0) {
		if (newContentLen > 0) {
			newContent = malloc(newContentLen * MAX_STORAGE_NAME_LEN);
			if (newContent == NULL) {
				fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
				return 1;
			}
			memcpy(newContent, file->content, newContentLen * MAX_STORAGE_NAME_LEN);
		}

		goto constructAction;
	}

	// determine which blocks have been changed -- one byte per block
	char* blocksToWrite = malloc(newContentLen);
	if (blocksToWrite == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		return 1;
	}
	memset(blocksToWrite, 0, newContentLen);

	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		determineBlocksToWrite(blocksToWrite, pw->offset, pw->size, newBlockSize);
	}

	// debug: print number of blocks to be written
	int blocksToWriteNum = 0;
	for (int i=0; i<newContentLen; i++) {
		blocksToWriteNum += blocksToWrite[i];
	}
	fprintf(stderr, "DEBUG: flush file: %d blocks to write\n", blocksToWriteNum);

	// construct and save new blocks (with destination->putStorageFile() calls)
	size_t maxEncryptedBlockSize = getMaxEncryptedBlockSize(newBlockSize);
	char* encryptedBlockBuf = malloc(maxEncryptedBlockSize);
	if (encryptedBlockBuf == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		free(blocksToWrite);
		return 2;
	}

	size_t maxDecryptedBlockSize = newBlockSize;
	char* decryptedBlockBuf = malloc(maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN);
	if (decryptedBlockBuf == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		free(blocksToWrite);
		free(encryptedBlockBuf);
		return 3;
	}
	newContent = malloc(newContentLen * MAX_STORAGE_NAME_LEN);
	if (newContent == NULL) {
		free(blocksToWrite);
		free(encryptedBlockBuf);
		free(decryptedBlockBuf);
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		return 4;
	}
	int ioerror = 0;
	for (int i=0; i<newContentLen; i++) {
		if (blocksToWrite[i] == 0) {
			memcpy(newContent + (MAX_STORAGE_NAME_LEN * i),
				file->content + (MAX_STORAGE_NAME_LEN * i),
				MAX_STORAGE_NAME_LEN);
			continue;
		}

		size_t encryptedBlockBufSize = maxEncryptedBlockSize;
		size_t decryptedBlockBufSize = maxDecryptedBlockSize;
		if (i < file->contentLen) {
			size_t expectedReadSize = file->size - (i * file->blockSize);
			if (expectedReadSize > file->blockSize) {
				expectedReadSize = file->blockSize;
			}

			const char* block = file->content + (MAX_STORAGE_NAME_LEN * i);
			int res = destination->getStorageFile(block, encryptedBlockBuf, &encryptedBlockBufSize);
			if (res != 0) {
				fprintf(stderr, "flushFile: getStorageFile failed for %s: %d\n",
					block, res);
				ioerror = 1;
				break;
			}

			res = encryption->decrypt(encryptedBlockBuf, encryptedBlockBufSize,
				decryptedBlockBuf, &decryptedBlockBufSize,
				conf.passphrase);
			if (res != 0) {
				fprintf(stderr, "flushFile: decrypt failed: %d\n", res);
				ioerror = 1;
				break;
			}
			if (decryptedBlockBufSize != expectedReadSize) {
				fprintf(stderr, "flushFile: expected decrypted block size %d, got %d\n",
					expectedReadSize, decryptedBlockBufSize);
				ioerror = 1;
				break;
			}
		}
		// right now we have data in decryptedBlockBuf
		
		// apply write operations
		for (int j=0; j<file->pendingWrites.len; j++) {
			PendingWrite* pw = file->pendingWrites.objects[j];
			int relOffset = pw->offset - i * newBlockSize;

			if (relOffset >= newBlockSize) {
				continue;
			} else if (relOffset + (int)pw->size <= 0) {
				continue;
			}

			if (relOffset <= 0) {
				int bytesToCopy = pw->size + relOffset;
				if (bytesToCopy > newBlockSize) {
					bytesToCopy = newBlockSize;
				}
				memcpy(decryptedBlockBuf, pw->buf - relOffset, bytesToCopy);
			} else {
				int bytesToCopy = pw->size;
				if (bytesToCopy > newBlockSize - relOffset) {
					bytesToCopy = newBlockSize - relOffset;
				}
				memcpy(decryptedBlockBuf + relOffset, pw->buf, bytesToCopy);
			}
		}

		size_t expectedWriteSize = newSize - (i * newBlockSize);
		if (expectedWriteSize > newBlockSize) {
			expectedWriteSize = newBlockSize;
		}
		encryptedBlockBufSize = maxEncryptedBlockSize;

		// encrypt
		int res = encryption->encrypt(decryptedBlockBuf, expectedWriteSize,
			encryptedBlockBuf, &encryptedBlockBufSize,
			conf.passphrase);
		if (res != 0) {
			fprintf(stderr, "flushFile: encrypt failed: %d\n", res);
			ioerror = 1;
			break;
		}

		// save
		char newStorageFileName[MAX_STORAGE_NAME_LEN];
		if (getRandomStorageFileName(newStorageFileName) != 0) {
			fprintf(stderr, "flushFile: getRandomStorageFileName failed\n");
			ioerror = 1;
			break;
		}
		res = destination->putStorageFile(newStorageFileName, encryptedBlockBuf, encryptedBlockBufSize);
		if (res != 0) {
			fprintf(stderr, "flushFile: putStorageFile failed: %d\n", res);
			ioerror = 1;
			break;
		}

		memcpy(newContent + (MAX_STORAGE_NAME_LEN * i),
			newStorageFileName,
			MAX_STORAGE_NAME_LEN);
	}

	free(encryptedBlockBuf);
	free(decryptedBlockBuf);
	free(blocksToWrite);

	if (ioerror) {
		if (newContent) {
			free(newContent);
		}
		return 5;
	}

constructAction:;

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		if (newContent) {
			free(newContent);
		}
		return 6;
	}
	newAction->time = getCurrentTime();

	if (file->dirtyFlags & DirtyFlagPendingCreate) {
		newAction->actionType = ActionTypeAddFile;
	}
	else {
		newAction->actionType = ActionTypeEditFile;
	}

	newAction->path = getFullFilePath(file);
	if (newAction->path == NULL) {
		fprintf(stderr, "flushFile: getFullFilePath() failed: %s\n", strerror(errno));
		if (newContent) {
			free(newContent);
		}
		free(newAction);
		return 7;
	}
	newAction->content = newContent;
	newAction->contentLen = newContentLen;
	newAction->size = newSize;
	newAction->blockSize = newBlockSize;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		fprintf(stderr, "flushFile: encryptAndAddActionFile failed\n");
		if (newContent) {
			free(newContent);
		}
		free(newAction->path);
		free(newAction);
		return 10;
	}

	// add to actions array
	addAction(newAction);

	// update file
	if (file->dirtyFlags & DirtyFlagPendingCreate) {
		// file->name was temporary created, now we need to set a proper
		// pointer to a data from newAction->path
		free((char*)file->name);
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		file->name = path_split(newAction->path, &pathArray);
		path_free(&pathArray);
	}
	file->mtime = newAction->time;
	file->content = newAction->content;
	file->contentLen = newAction->contentLen;
	file->size = newAction->size;
	file->blockSize = newAction->blockSize;
	file->dirtyFlags = DirtyFlagNotDirty;

	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		free(pw->buf);
		free(pw);
	}
	freeDynArray(&file->pendingWrites);

	return 0;
}

static int bucse_flush(const char *path, struct fuse_file_info *fi)
{
	(void) fi;

	fprintf(stderr, "DEBUG: flush %s\n", path);

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
			fprintf(stderr, "bucse_flush: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_flush: path not found when writing file %s\n", path);
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

	if (file->dirtyFlags & DirtyFlagPendingWrite) {
		flushFile(file);
	}

	return 0;
}

int bucse_flush_guarded(const char *path, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_flush(path, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


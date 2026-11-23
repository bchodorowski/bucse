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

#define MIN_BLOCK_SIZE 512
#define MAX_BLOCK_SIZE (128 * 1024 * 1024)

#define RESIZE_AT_BLOCKS_COUNT 32

extern Destination *destination;
extern Encryption *encryption;

// determine block size using a file size
static int getBlockSize(size_t size)
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
	// the operations reject changes in readOnly mode, this is the last
	// guard against writing to the repository anyway
	if (confIsReadOnly()) {
		logPrintf(LOG_ERROR, "flushFile: cannot do that in readOnly mode\n");
		return -EROFS;
	}
	// TODO: what if create only, no writes?

	size_t newSize = file->size;
	// truncSize is kept until the flush succeeds (the flag is cleared then),
	// so a failed flush can be retried with the same size
	if (file->dirtyFlags & DirtyFlagPendingTrunc) {
		newSize = file->truncSize;
	}
	// the old data that is kept: after a shrinking truncate, the bytes
	// past truncSize must read as zeroes if a write extends the file again
	size_t validSize = file->size;
	if (newSize < validSize) {
		validSize = newSize;
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

	// when the file shrinks and its new last block is partial, that block
	// is shorter than the stored one and has to be rewritten
	int lastBlockShrinks = newBlockSize > 0 && newSize < file->size
		&& newSize % newBlockSize != 0;

	// if no block needs to be written
	if (file->pendingWrites.len == 0 && !lastBlockShrinks) {
		if (newContentLen > 0) {
			newContent = malloc(newContentLen * MAX_STORAGE_NAME_LEN);
			if (newContent == NULL) {
				logPrintf(LOG_ERROR, "flushFile: malloc(): %s\n", strerror(errno));
				return 1;
			}
			memcpy(newContent, file->content, newContentLen * MAX_STORAGE_NAME_LEN);
		}

		goto constructAction;
	}

	// if newContentLen is too big, change the blockSize and rewrite the whole file
	int resizeContent = 0;
	if (newContentLen > RESIZE_AT_BLOCKS_COUNT && newBlockSize < MAX_BLOCK_SIZE) {
		logPrintf(LOG_DEBUG, "flushFile: resize blocks\n");
		newBlockSize = getBlockSize(newSize);
		newContentLen = (int)(newSize / (size_t)newBlockSize)
			+ (int)(newSize % newBlockSize != 0);
		resizeContent = 1;
	}

	// determine which blocks have been changed -- one byte per block
	char* blocksToWrite = malloc(newContentLen);
	if (blocksToWrite == NULL) {
		logPrintf(LOG_ERROR, "flushFile: malloc(): %s\n", strerror(errno));
		return 1;
	}

	if (resizeContent) {
		memset(blocksToWrite, 1, newContentLen);
	} else {
		memset(blocksToWrite, 0, newContentLen);
	}

	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		determineBlocksToWrite(blocksToWrite, pw->offset, pw->size, newBlockSize);
	}

	// when there are writes past the kept data (validSize), blocks from
	// the one containing validSize up to the end may not have been
	// touched by them. They must be rewritten (not copied over), because
	// the bytes past validSize must be zeroes (e.g. a shrinking truncate
	// followed by a write past the new end)
	if (validSize < newSize) {
		for (int i = (int)(validSize / newBlockSize); i < newContentLen; i++) {
			blocksToWrite[i] = 1;
		}
	}

	if (lastBlockShrinks) {
		blocksToWrite[newContentLen - 1] = 1;
	}
	
	// debug: print number of blocks to be written
	int blocksToWriteNum = 0;
	for (int i=0; i<newContentLen; i++) {
		blocksToWriteNum += blocksToWrite[i];
	}
	logPrintf(LOG_DEBUG, "flush file: %d blocks to write\n", blocksToWriteNum);

	// construct and save new blocks (with destination->putStorageFile() calls)
	size_t maxEncryptedBlockSize = getMaxEncryptedBlockSize(newBlockSize);
	char* encryptedBlockBuf = malloc(maxEncryptedBlockSize);
	if (encryptedBlockBuf == NULL) {
		logPrintf(LOG_ERROR, "flushFile: malloc(): %s\n", strerror(errno));
		free(blocksToWrite);
		return 2;
	}

	size_t maxDecryptedBlockSize = newBlockSize;
	char* decryptedBlockBuf = malloc(maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN);
	if (decryptedBlockBuf == NULL) {
		logPrintf(LOG_ERROR, "flushFile: malloc(): %s\n", strerror(errno));
		free(blocksToWrite);
		free(encryptedBlockBuf);
		return 3;
	}
	newContent = malloc(newContentLen * MAX_STORAGE_NAME_LEN);
	if (newContent == NULL) {
		free(blocksToWrite);
		free(encryptedBlockBuf);
		free(decryptedBlockBuf);
		logPrintf(LOG_ERROR, "flushFile: malloc(): %s\n", strerror(errno));
		return 4;
	}
	int ioerror = 0;
	for (int i=0; i<newContentLen; i++) {
		if (blocksToWrite[i] == 0) {
			if (i < file->contentLen) {
				memcpy(newContent + (MAX_STORAGE_NAME_LEN * i),
					file->content + (MAX_STORAGE_NAME_LEN * i),
					MAX_STORAGE_NAME_LEN);
				continue;
			}
		}

		memset(decryptedBlockBuf, 0, maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN);
		size_t encryptedBlockBufSize = maxEncryptedBlockSize;
		size_t decryptedBlockBufSize = maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN;

		// special case when we need to resize blocks
		if (resizeContent) {
			logPrintf(LOG_VERBOSE_DEBUG, "content needs to be resized\n");
			size_t toRead = newSize - ((size_t)i * newBlockSize);
			if (toRead > newBlockSize) {
				toRead = newBlockSize;
			}
			// we are using the fact that the new block size is a multiple of the old block size
			int sourceBlockIndex = (newBlockSize/file->blockSize)*i;
			char* bufPtr = decryptedBlockBuf;
			size_t bufSize = maxDecryptedBlockSize;
			while (toRead > 0) {
				if ((size_t)sourceBlockIndex * file->blockSize >= validSize) {
					logPrintf(LOG_VERBOSE_DEBUG, "toRead fill with zeros: %zu\n", toRead);
					memset(bufPtr, 0, toRead);
					toRead = 0;
					break;
				}
				logPrintf(LOG_VERBOSE_DEBUG, "toRead: %zu\n", toRead);
				size_t expectedReadSize = file->size - ((size_t)sourceBlockIndex * file->blockSize);
				if (expectedReadSize > file->blockSize) {
					expectedReadSize = file->blockSize;
				}
				const char* block = file->content + (MAX_STORAGE_NAME_LEN * sourceBlockIndex);
				logPrintf(LOG_VERBOSE_DEBUG, "expectedReadSize: %zu\n", expectedReadSize);
				logPrintf(LOG_VERBOSE_DEBUG, "bufSize: %zu\n", bufSize);
				logPrintf(LOG_VERBOSE_DEBUG, "sourceBlockIndex: %d\n", sourceBlockIndex);
				logPrintf(LOG_VERBOSE_DEBUG, "block: %s\n", block);

				// a longer block is fine (see below), the bytes past
				// expectedReadSize are overwritten by the next block or
				// zero-filled
				size_t bufSizeCopy = bufSize + DECRYPTED_BUFFER_MARGIN;
				size_t encryptedBlockBufSizeCopy = encryptedBlockBufSize;
				if (decryptBlock(block,
					bufPtr, &bufSizeCopy,
					encryptedBlockBuf, &encryptedBlockBufSizeCopy,
					expectedReadSize) != 0) {
					ioerror = 1;
					break;
				}
				bufPtr += expectedReadSize;
				bufSize -= expectedReadSize;
				toRead -= expectedReadSize;
				sourceBlockIndex++;
			}
			if (ioerror) {
				break;
			}
		}
		else if (i < file->contentLen && (size_t)i * newBlockSize < validSize) {
			size_t expectedReadSize = file->size - ((size_t)i * file->blockSize);
			if (expectedReadSize > file->blockSize) {
				expectedReadSize = file->blockSize;
			}

			const char* block = file->content + (MAX_STORAGE_NAME_LEN * i);
			
			// Versions before 2026-11 did not rewrite the last block
			// when truncating, so it may be longer than expected. Accept
			// it and zero everything after the file's end, which must
			// read as zeroes if the file gets extended.
			if (decryptBlock(block,
				decryptedBlockBuf, &decryptedBlockBufSize,
				encryptedBlockBuf, &encryptedBlockBufSize,
				expectedReadSize) != 0) {
				ioerror = 1;
				break;
			}
		}

		// zero everything past the kept data (validSize): the rest of a
		// longer block, or bytes cut off by a pending truncate
		size_t blockStart = (size_t)i * newBlockSize;
		if (blockStart + newBlockSize > validSize) {
			size_t keep = validSize > blockStart ? validSize - blockStart : 0;
			memset(decryptedBlockBuf + keep, 0,
				maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN - keep);
		}
		// right now we have data in decryptedBlockBuf
		
		// apply write operations
		for (int j=0; j<file->pendingWrites.len; j++) {
			PendingWrite* pw = file->pendingWrites.objects[j];
			off_t relOffset = pw->offset - (off_t)i * newBlockSize;

			if (relOffset >= newBlockSize) {
				continue;
			} else if (relOffset + (off_t)pw->size <= 0) {
				continue;
			}

			if (relOffset <= 0) {
				off_t bytesToCopy = (off_t)pw->size + relOffset;
				if (bytesToCopy > newBlockSize) {
					bytesToCopy = newBlockSize;
				}
				memcpy(decryptedBlockBuf, pw->buf - relOffset, bytesToCopy);
			} else {
				off_t bytesToCopy = (off_t)pw->size;
				if (bytesToCopy > newBlockSize - relOffset) {
					bytesToCopy = newBlockSize - relOffset;
				}
				memcpy(decryptedBlockBuf + relOffset, pw->buf, bytesToCopy);
			}
		}

		size_t expectedWriteSize = newSize - ((size_t)i * newBlockSize);
		if (expectedWriteSize > newBlockSize) {
			expectedWriteSize = newBlockSize;
		}
		encryptedBlockBufSize = maxEncryptedBlockSize;

		// encrypt
		int res = encryption->encrypt(decryptedBlockBuf, expectedWriteSize,
			encryptedBlockBuf, &encryptedBlockBufSize,
			conf.passphrase);
		if (res != 0) {
			logPrintf(LOG_ERROR, "flushFile: encrypt failed: %d\n", res);
			ioerror = 1;
			break;
		}

		// save
		char newStorageFileName[MAX_STORAGE_NAME_LEN];
		if (getRandomStorageFileName(newStorageFileName) != 0) {
			logPrintf(LOG_ERROR, "flushFile: getRandomStorageFileName failed\n");
			ioerror = 1;
			break;
		}
		res = destination->putStorageFile(newStorageFileName, encryptedBlockBuf, encryptedBlockBufSize);
		if (res != 0) {
			logPrintf(LOG_ERROR, "flushFile: putStorageFile failed: %d\n", res);
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

	// another client may have removed the file's directory
	if (recreatePendingDirs(file->parentDir) != 0) {
		if (newContent) {
			free(newContent);
		}
		return 11;
	}

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		logPrintf(LOG_ERROR, "flushFile: malloc(): %s\n", strerror(errno));
		if (newContent) {
			free(newContent);
		}
		return 6;
	}
	newAction->time = getNewActionTime();

	if (file->dirtyFlags & DirtyFlagPendingCreate) {
		newAction->actionType = ActionTypeAddFile;
	}
	else {
		newAction->actionType = ActionTypeEditFile;
	}

	newAction->path = getFullFilePath(file);
	if (newAction->path == NULL) {
		logPrintf(LOG_ERROR, "flushFile: getFullFilePath() failed: %s\n", strerror(errno));
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
		logPrintf(LOG_ERROR, "flushFile: encryptAndAddActionFile failed\n");
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
	file->pendingWritesSize = 0;

	return 0;
}

static int bucse_flush(const char *path, struct fuse_file_info *fi)
{
	(void) fi;

	logPrintf(LOG_DEBUG, "flush %s\n", path);

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
			logPrintf(LOG_ERROR, "bucse_flush: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			logPrintf(LOG_ERROR, "bucse_flush: path not found when writing file %s\n", path);
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
		if (flushFile(file) != 0) {
			return -EIO;
		}
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


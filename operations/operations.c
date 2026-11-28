#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>

#include "../actions.h"
#include "../dynarray.h"
#include "../filesystem.h"
#include "../time.h"
#include "../destinations/dest.h"
#include "../encryption/encr.h"
#include "../log.h"
#include "../conf.h"
#include "../cache.h"

#include "operations.h"

pthread_mutex_t bucseMutex;

extern Destination *destination;
extern Encryption *encryption;

int encryptAndAddActionFile(Action* newAction)
{
	return encryptAndAddActionFiles(&newAction, 1);
}

int encryptAndAddActionFiles(Action** newActions, size_t count)
{
	// once the file is written, the caller adds its actions with
	// addAction(), which then can't fail
	if (reserveActions((int)count) != 0) {
		logPrintf(LOG_ERROR, "encryptAndAddActionFile: reserveActions() failed\n");
		return -5;
	}

	char* jsonData = serializeActions(newActions, count);
	if (jsonData == NULL) {
		logPrintf(LOG_ERROR, "encryptAndAddActionFile: serializeActions() failed\n");
		return -1;
	}

	char newActionFileName[MAX_STORAGE_NAME_LEN];
	if (getRandomStorageFileName(newActionFileName) != 0) {
		logPrintf(LOG_ERROR, "encryptAndAddActionFile: getRandomStorageFileName failed\n");
		free(jsonData);
		return -2;
	}

	size_t encryptedBufLen = 2 * MAX_ACTION_LEN;
	char* encryptedBuf = malloc(encryptedBufLen);
	if (encryptedBuf == NULL) {
		logPrintf(LOG_ERROR, "encryptAndAddActionFile: malloc(): %s\n", strerror(errno));
		free(jsonData);
		return -3;
	}
	int result = encryption->encrypt(jsonData, strlen(jsonData),
		encryptedBuf, &encryptedBufLen,
		conf.passphrase);

	if (result != 0) {
		logPrintf(LOG_ERROR, "encryptAndAddActionFile: encrypt failed: %d\n", result);
		free(jsonData);
		free(encryptedBuf);
		return -4;
	}

	result = destination->addActionFile(newActionFileName, encryptedBuf, encryptedBufLen);
	free(jsonData);
	free(encryptedBuf);
	return result;
}

int recreatePendingDirs(FilesystemDir* dir)
{
	if (dir == NULL) {
		return 0;
	}

	int result = recreatePendingDirs(dir->parentDir);
	if (result != 0 || !dir->pendingCreate) {
		return result;
	}

	Action* newAction = calloc(1, sizeof(Action));
	if (newAction == NULL) {
		logPrintf(LOG_ERROR, "recreatePendingDirs: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getNewActionTime();
	newAction->actionType = ActionTypeAddDirectory;
	newAction->path = getFullDirPath(dir);
	if (newAction->path == NULL) {
		logPrintf(LOG_ERROR, "recreatePendingDirs: getFullDirPath() failed\n");
		free(newAction);
		return -ENOMEM;
	}
	newAction->content = NULL;
	newAction->contentLen = 0;
	newAction->size = 0;
	newAction->blockSize = 0;

	if (encryptAndAddActionFile(newAction) != 0) {
		logPrintf(LOG_ERROR, "recreatePendingDirs: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		return -EIO;
	}

	logPrintf(LOG_WARNING, "directory %s was removed by another client, added it again\n",
		newAction->path);

	// dir->name still points to the path of the action that added the dir
	// first; actions are kept until bucse-mount exits
	addAction(newAction);
	dir->pendingCreate = 0;
	return 0;
}

int decryptBlock(const char* block, size_t maxBlockSize, size_t expectedReadSize,
	const char** data, size_t* dataSize)
{
	const char* cachedData;
	size_t cachedSize;
	if (cacheGet(block, &cachedData, &cachedSize) != 0) {
		size_t encryptedBufSize = getMaxEncryptedBlockSize(maxBlockSize);
		char* encryptedBuf = malloc(encryptedBufSize);
		if (encryptedBuf == NULL) {
			logPrintf(LOG_ERROR, "decryptBlock: malloc(): %s\n", strerror(errno));
			return 6;
		}
		int res = destination->getStorageFile(block, encryptedBuf, &encryptedBufSize);
		if (res != 0) {
			logPrintf(LOG_ERROR, "decryptBlock: getStorageFile failed for %s: %d\n",
					block, res);
			free(encryptedBuf);
			return 1;
		}

		size_t decryptedBufSize = maxBlockSize + DECRYPTED_BUFFER_MARGIN;
		char* decryptedBuf = malloc(decryptedBufSize);
		if (decryptedBuf == NULL) {
			logPrintf(LOG_ERROR, "decryptBlock: malloc(): %s\n", strerror(errno));
			free(encryptedBuf);
			return 6;
		}
		res = encryption->decrypt(encryptedBuf, encryptedBufSize,
				decryptedBuf, &decryptedBufSize,
				conf.passphrase);
		free(encryptedBuf);
		if (res != 0) {
			logPrintf(LOG_ERROR, "decryptBlock: decrypt failed: %d\n", res);
			free(decryptedBuf);
			return 2;
		}

		if (decryptedBufSize > maxBlockSize) {
			logPrintf(LOG_ERROR, "decryptBlock: decrypted block too large: %zu, at most %zu expected\n",
					decryptedBufSize, maxBlockSize);
			free(decryptedBuf);
			return 5;
		}

		// the cache takes the buffer: give back what the margin and
		// a shorter (last) block don't use
		char* shrunk = realloc(decryptedBuf, decryptedBufSize > 0 ? decryptedBufSize : 1);
		if (shrunk != NULL) {
			decryptedBuf = shrunk;
		}
		if (cachePut(block, decryptedBuf, decryptedBufSize) != 0) {
			logPrintf(LOG_ERROR, "decryptBlock: cachePut failed\n");
			free(decryptedBuf);
			return 7;
		}
		cachedData = decryptedBuf;
		cachedSize = decryptedBufSize;
	}

	// checked on every call, not only when the block is fetched: a cached
	// block may be needed by a call that expects more of it, or allows less
	if (cachedSize > maxBlockSize) {
		logPrintf(LOG_ERROR, "decryptBlock: decrypted block too large: %zu, at most %zu expected\n",
				cachedSize, maxBlockSize);
		return 5;
	}
	if (cachedSize < expectedReadSize) {
		logPrintf(LOG_ERROR, "decryptBlock: expected decrypted block size at least %zu, got %zu\n",
				expectedReadSize, cachedSize);
		return 3;
	}

	*data = cachedData;
	*dataSize = cachedSize;
	return 0;
}

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
	char* jsonData = serializeAction(newAction);
	if (jsonData == NULL) {
		logPrintf(LOG_ERROR, "encryptAndAddActionFile: serializeAction() failed\n");
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

	Action* newAction = malloc(sizeof(Action));
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

int decryptBlock(const char* block,
	char* decryptedBlockBuf, size_t* decryptedBlockBufSize,
	char* encryptedBlockBuf, size_t* encryptedBlockBufSize,
	size_t expectedReadSize)
{
	if (*decryptedBlockBufSize < DECRYPTED_BUFFER_MARGIN) {
		logPrintf(LOG_ERROR, "decryptBlock: buffer too small\n");
		return 4;
	}
	size_t maxBlockSize = *decryptedBlockBufSize - DECRYPTED_BUFFER_MARGIN;

	if (cacheGet(block, decryptedBlockBuf, decryptedBlockBufSize) != 0) {
		int res = destination->getStorageFile(block, encryptedBlockBuf, encryptedBlockBufSize);
		if (res != 0) {
			logPrintf(LOG_ERROR, "decryptBlock: getStorageFile failed for %s: %d\n",
					block, res);
			return 1;
		}

		res = encryption->decrypt(encryptedBlockBuf, *encryptedBlockBufSize,
				decryptedBlockBuf, decryptedBlockBufSize,
				conf.passphrase);
		if (res != 0) {
			logPrintf(LOG_ERROR, "decryptBlock: decrypt failed: %d\n", res);
			return 2;
		}

		if (*decryptedBlockBufSize > maxBlockSize) {
			logPrintf(LOG_ERROR, "decryptBlock: decrypted block too large: %zu, at most %zu expected\n",
					*decryptedBlockBufSize, maxBlockSize);
			return 5;
		}

		if (*decryptedBlockBufSize < expectedReadSize) {
			logPrintf(LOG_ERROR, "decryptBlock: expected decrypted block size at least %zu, got %zu\n",
					expectedReadSize, *decryptedBlockBufSize);
			return 3;
		}

		cachePut(block, decryptedBlockBuf, *decryptedBlockBufSize);
	}
	return 0;
}

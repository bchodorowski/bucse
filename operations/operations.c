#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>

#include "../actions.h"
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

int decryptBlock(const char* block,
	char* decryptedBlockBuf, size_t* decryptedBlockBufSize,
	char* encryptedBlockBuf, size_t* encryptedBlockBufSize,
	int exactly,
	size_t expectedReadSize)
{
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

		if (exactly) {
			if (*decryptedBlockBufSize != expectedReadSize) {
				logPrintf(LOG_ERROR, "decryptBlock: expected decrypted block size %d, got %d\n",
						expectedReadSize, *decryptedBlockBufSize);
				return 3;
			}
		} else {
			if (*decryptedBlockBufSize < expectedReadSize) {
				logPrintf(LOG_ERROR, "decryptBlock: expected decrypted block size at least %d, got %d\n",
						expectedReadSize, *decryptedBlockBufSize);
				return 3;
			}
		}

		cachePut(block, decryptedBlockBuf, *decryptedBlockBufSize);
	}
	return 0;
}

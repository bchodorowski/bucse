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
#include "../conf.h"

#include "operations.h"

pthread_mutex_t bucseMutex;

extern Destination *destination;
extern Encryption *encryption;

int encryptAndAddActionFile(Action* newAction)
{
	char* jsonData = serializeAction(newAction);
	if (jsonData == NULL) {
		fprintf(stderr, "encryptAndAddActionFile: serializeAction() failed\n");
		return -1;
	}

	char newActionFileName[MAX_STORAGE_NAME_LEN];
	if (getRandomStorageFileName(newActionFileName) != 0) {
		fprintf(stderr, "encryptAndAddActionFile: getRandomStorageFileName failed\n");
		free(jsonData);
		return -2;
	}

	size_t encryptedBufLen = 2 * MAX_ACTION_LEN;
	char* encryptedBuf = malloc(encryptedBufLen);
	if (encryptedBuf == NULL) {
		fprintf(stderr, "encryptAndAddActionFile: malloc(): %s\n", strerror(errno));
		free(jsonData);
		return -3;
	}
	int result = encryption->encrypt(jsonData, strlen(jsonData),
		encryptedBuf, &encryptedBufLen,
		conf.passphrase);

	if (result != 0) {
		fprintf(stderr, "encryptAndAddActionFile: encrypt failed: %d\n", result);
		free(jsonData);
		free(encryptedBuf);
		return -4;
	}

	result = destination->addActionFile(newActionFileName, encryptedBuf, encryptedBufLen);
	free(jsonData);
	free(encryptedBuf);
	return result;
}

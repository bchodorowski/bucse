#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../log.h"

#include "encr.h"

extern Encryption encryptionNone;
extern Encryption encryptionAes;

size_t getMaxEncryptedBlockSize(size_t blockSize)
{
	blockSize *= 2;
	if (blockSize < 256) {
		blockSize = 256;
	}
	return blockSize;
}

Encryption* getEncryptionByName(char* name)
{
	if (name == NULL || strcmp(name, "none") == 0) {
		return &encryptionNone;
	} else if (strcmp(name, "aes") == 0) {
		return &encryptionAes;
	} else {
		logPrintf(LOG_ERROR, "getEncryptionByName:() Unsupported encryption: %s\n", name);
		return NULL;
	}
}

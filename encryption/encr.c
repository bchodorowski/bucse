#include <stddef.h>

#include "encr.h"

size_t getMaxEncryptedBlockSize(size_t blockSize)
{
	blockSize *= 2;
	if (blockSize < 256) {
		blockSize = 256;
	}
	return blockSize;
}


#include <stddef.h>

#include "encr.h"

int encrNoneEncrypt(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *key)
{
	return -1;
}

int encrNoneDecrypt(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *key)
{
	return -1;
}

Encryption encryptionNone = {
	.encrypt = encrNoneEncrypt,
	.decrypt = encrNoneDecrypt
};

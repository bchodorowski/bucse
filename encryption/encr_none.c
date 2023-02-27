#include <stddef.h>
#include <string.h>

#include "encr.h"

int encrNoneEncrypt(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *passphrase)
{
	(void) passphrase;
	memcpy(outBuf, inBuf, inSize);
	*outSize = inSize;
	return 0;
}

int encrNoneDecrypt(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *passphrase)
{
	(void) passphrase;
	memcpy(outBuf, inBuf, inSize);
	*outSize = inSize;
	return 0;
}

int encrNoneNeedsPassphrase()
{
	return 0;
}

Encryption encryptionNone = {
	.encrypt = encrNoneEncrypt,
	.decrypt = encrNoneDecrypt,
	.needsPassphrase = encrNoneNeedsPassphrase
};

#include <stddef.h>
#include <string.h>
#include <errno.h>

#include <openssl/evp.h>

#include "../log.h"

#include "encr.h"

// try to implement something that works like
// openssl enc -aes-256-cbc -in TODO -md sha1 -iter 1 > TODO.aes

int encrAesEncrypt(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *pass)
{
	EVP_CIPHER_CTX *ctx;

	if (!(ctx = EVP_CIPHER_CTX_new())) {
		logPrintf(LOG_ERROR, "encrAesEncrypt: EVP_CIPHER_CTX_new failed\n");
		return 1;
	}
	unsigned char salt[8];
	unsigned char key[32];
	unsigned char iv[32];
	
	FILE* f = fopen("/dev/urandom", "rb");
	if (f == NULL) {
		logPrintf(LOG_ERROR, "encrAesEncrypt: fopen(): %s\n", strerror(errno));
		return 2;
	}

	size_t got = fread(salt, 8, 1, f);
	if (got == 0) {
		logPrintf(LOG_ERROR, "encrAesEncrypt: fread failed\n");
		fclose(f);
		return 3;
	}

	fclose(f);

	unsigned char keyiv[64];
	PKCS5_PBKDF2_HMAC_SHA1((const char*)pass, strlen(pass),
		salt, 8, 1, sizeof(keyiv), keyiv);
	memcpy(key, keyiv, sizeof(key));
	memcpy(iv, keyiv+sizeof(key), sizeof(iv));
	
	outBuf += 16; // make space for Salted__ header
	*outSize -= 16;

	if (EVP_EncryptInit(ctx, EVP_aes_256_cbc(), key, iv) != 1) {
		logPrintf(LOG_ERROR, "encrAesEncrypt: EVP_EncryptInit failed\n");
		EVP_CIPHER_CTX_free(ctx);
		return 4;
	}

	EVP_CIPHER_CTX_set_key_length(ctx, EVP_MAX_KEY_LENGTH);

	if (EVP_EncryptUpdate(ctx, (unsigned char*)outBuf, (int*)outSize, (unsigned char*)inBuf, inSize) != 1) {
		logPrintf(LOG_ERROR, "encrAesEncrypt: EVP_EncryptUpdate failed\n");
		EVP_CIPHER_CTX_free(ctx);
		return 5;
	}

	int tmpLen = 0;

	if (!EVP_EncryptFinal(ctx, (unsigned char*)(outBuf + *outSize), &tmpLen)) {
		logPrintf(LOG_ERROR, "encrAesEncrypt: EVP_EncryptFinal failed\n");
		EVP_CIPHER_CTX_free(ctx);
		return 6;
	}
	*outSize += tmpLen;

	EVP_CIPHER_CTX_free(ctx);

	memcpy(outBuf - 16, "Salted__", 8);
	memcpy(outBuf - 8, salt, 8);

	*outSize += 16;

	return 0;
}

int encrAesDecrypt(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *pass)
{
	EVP_CIPHER_CTX *ctx;

	if (!(ctx = EVP_CIPHER_CTX_new())) {
		logPrintf(LOG_ERROR, "encrAesDecrypt: EVP_CIPHER_CTX_new failed\n");
		return 1;
	}
	unsigned char salt[8];
	unsigned char key[32];
	unsigned char iv[32];
	
	if (inSize >= 16 && strncmp((const char*)inBuf, "Salted__", 8) == 0) {
		memcpy(salt, &inBuf[8], 8);
		inBuf += 16;
		inSize -= 16;
	} else {
		EVP_CIPHER_CTX_free(ctx);
		return 2;
	}

	unsigned char keyiv[64];
	PKCS5_PBKDF2_HMAC_SHA1((const char*)pass, strlen(pass),
		salt, 8, 1, sizeof(keyiv), keyiv);
	memcpy(key, keyiv, sizeof(key));
	memcpy(iv, keyiv+sizeof(key), sizeof(iv));

	if (EVP_DecryptInit(ctx, EVP_aes_256_cbc(), key, iv) != 1) {
		logPrintf(LOG_ERROR, "encrAesDecrypt: EVP_DecryptInit failed\n");
		EVP_CIPHER_CTX_free(ctx);
		return 3;
	}

	EVP_CIPHER_CTX_set_key_length(ctx, EVP_MAX_KEY_LENGTH);

	if (EVP_DecryptUpdate(ctx, (unsigned char*)outBuf, (int*)outSize, (unsigned char*)inBuf, inSize) != 1) {
		logPrintf(LOG_ERROR, "encrAesDecrypt: EVP_DecryptUpdate failed\n");
		EVP_CIPHER_CTX_free(ctx);
		return 4;
	}

	int tmpLen = 0;

	if (!EVP_DecryptFinal(ctx, (unsigned  char*)(outBuf + *outSize), &tmpLen)) {
		logPrintf(LOG_ERROR, "encrAesDecrypt: EVP_DecryptFinal failed\n");
		EVP_CIPHER_CTX_free(ctx);
		return 5;
	}
	*outSize += tmpLen;

	EVP_CIPHER_CTX_free(ctx);

	return 0;
}

int encrAesNeedsPassphrase()
{
	return 1;
}

Encryption encryptionAes = {
	.encrypt = encrAesEncrypt,
	.decrypt = encrAesDecrypt,
	.needsPassphrase = encrAesNeedsPassphrase
};


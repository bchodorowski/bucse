// We need a bit larger buffer for decryption due to how EVP_DecryptUpdate() works.
// Quote from https://www.openssl.org/docs/man1.1.1/man3/EVP_DecryptUpdate.html
//
// > The parameters and restrictions are identical to the encryption operations
// > except that if padding is enabled the decrypted data buffer out passed to
// > EVP_DecryptUpdate() should have sufficient room for (inl +
// > cipher_block_size) bytes unless the cipher block size is 1 in which case inl
// > bytes is sufficient. 
#define DECRYPTED_BUFFER_MARGIN 16

typedef struct {
	int (*encrypt)(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *passphrase);
	int (*decrypt)(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *passphrase);

	int (*needsPassphrase)();
} Encryption;

size_t getMaxEncryptedBlockSize(size_t blockSize);

Encryption* getEncryptionByName(char* name);

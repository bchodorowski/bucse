extern pthread_mutex_t bucseMutex;

int encryptAndAddActionFile(Action* newAction);

// auxiliary function that decrypts a block and verifies the read size
int decryptBlock(const char* block,
	char* decryptedBlockBuf, size_t* decryptedBlockBufSize,
	char* encryptedBlockBuf, size_t* encryptedBlockBufSize,
	int exactly,
	size_t expectedReadSize);

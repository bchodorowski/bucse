extern pthread_mutex_t bucseMutex;

int encryptAndAddActionFile(Action* newAction);

// auxiliary function that decrypts a block and verifies that it has at least
// expectedReadSize bytes
int decryptBlock(const char* block,
	char* decryptedBlockBuf, size_t* decryptedBlockBufSize,
	char* encryptedBlockBuf, size_t* encryptedBlockBufSize,
	size_t expectedReadSize);

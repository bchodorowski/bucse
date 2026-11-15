extern pthread_mutex_t bucseMutex;

int encryptAndAddActionFile(Action* newAction);

// auxiliary function that decrypts a block and verifies that it has at least
// expectedReadSize bytes
// *decryptedBlockBufSize is the size of decryptedBlockBuf on input (including
// DECRYPTED_BUFFER_MARGIN) and the size of the decrypted block on output. A
// block longer than *decryptedBlockBufSize - DECRYPTED_BUFFER_MARGIN bytes is
// rejected: a valid block is never longer than the file's block size.
int decryptBlock(const char* block,
	char* decryptedBlockBuf, size_t* decryptedBlockBufSize,
	char* encryptedBlockBuf, size_t* encryptedBlockBufSize,
	size_t expectedReadSize);

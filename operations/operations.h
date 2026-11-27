extern pthread_mutex_t bucseMutex;

int encryptAndAddActionFile(Action* newAction);
// writes all of newActions in one action file, so every client applies
// them together (or none of them, if the file never gets complete)
int encryptAndAddActionFiles(Action** newActions, size_t count);

// Writes an addDirectory action for dir and each of its parents that has
// pendingCreate set (another client removed them while they held local data),
// topmost first. Call it before writing an action for anything inside dir.
int recreatePendingDirs(FilesystemDir* dir);

// Gets a decrypted block: from the cache, or from the destination (it is
// then decrypted and put to the cache). A block longer than maxBlockSize or
// shorter than expectedReadSize bytes is rejected: a valid block is never
// longer than the file's block size, and it holds the file's data up to the
// file's size. On success *data points to the block's data, *dataSize is its
// size. The data is owned by the cache: it stays valid only until the next
// decryptBlock() or cache call, so copy what is needed right away.
int decryptBlock(const char* block, size_t maxBlockSize, size_t expectedReadSize,
	const char** data, size_t* dataSize);

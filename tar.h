// The result of the callback for an entry that would be accepted if it was
// read again (e.g. there was no memory): forEveryFileInTar() then stops and
// fails, so that the whole tar file is read again. Any other non-zero result
// rejects the entry, which is then ignored: the tar file is complete, so
// reading it again wouldn't help.
#define TAR_ENTRY_RETRY 1000

int forEveryFileInTar(char* tarBuf, size_t tarSize, int (*actionAddedDecryptOneAction)(char*, char*, size_t));

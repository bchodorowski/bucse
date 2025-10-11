int forEveryFileInTar(char* tarBuf, size_t tarSize, int moreInThisBatch, void (*actionAddedDecryptOneAction)(char*, char*, size_t, int));

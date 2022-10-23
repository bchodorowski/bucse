typedef struct {
	int (*encrypt)(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *key);
	int (*decrypt)(char *inBuf, size_t inSize, char *outBuf, size_t *outSize, char *key);
} Encryption;

typedef void (*ActionAddedCallback)(char* actionName, char* buf, size_t size, int moreInThisBatch);

typedef struct {
	int (*init)(char* repository);
	void (*shutdown)();
	int (*putStorageFile)(const char* filename, char *buf, size_t size);
	int (*getStorageFile)(const char* filename, char *buf, size_t *size);
	int (*addActionFile)(char* filename, char *buf, size_t size);
	int (*getRepositoryFile)(char *buf, size_t *size);
	int (*setCallbackActionAdded)(ActionAddedCallback callback);
	int (*isTickable)();
	int (*tick)();
} Destination;

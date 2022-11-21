typedef void (*ActionAddedCalback)(char* filename, char* buf, size_t size);

typedef struct {
	int (*init)(char* repository);
	void (*shutdown)();
	int (*putStorageFile)(char* filename, char *buf, size_t size);
	int (*getStorageFile)(char* filename, char *buf, size_t *size);
	int (*addActionFile)(char* filename, char *buf, size_t size);
	int (*getRepositoryFile)(char *buf, size_t *size);
	int (*setCallbackActionAdded)(ActionAddedCalback callback);
	int (*isTickable)();
	int (*tick)();
} Destination;

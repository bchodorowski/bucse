#define MAX_FILEPATH_LEN 1024

#define MAX_ACTION_LEN (1024 * 1024)
#define MAX_ACTION_NAME_LEN 64

typedef void (*ActionAddedCallback)(char* actionName, char* buf, size_t size, int moreInThisBatch);

typedef struct {
	int (*init)(char* repository);
	void (*shutdown)();
	int (*putStorageFile)(const char* filename, char *buf, size_t size);
	int (*getStorageFile)(const char* filename, char *buf, size_t *size);
	int (*addActionFile)(char* filename, char *buf, size_t size);
	int (*getRepositoryJsonFile)(char *buf, size_t *size);
	int (*getRepositoryFile)(char *buf, size_t *size);
	int (*setCallbackActionAdded)(ActionAddedCallback callback);
	int (*isTickable)();
	int (*tick)();
} Destination;

int getRandomStorageFileName(char* filename);

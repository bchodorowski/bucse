#define MAX_STORAGE_NAME_LEN 64

// a file's block size is a power of 2 in this range (0 for a file that never
// had any data); an addFile or editFile action with another one is rejected
#define MIN_BLOCK_SIZE 512
#define MAX_BLOCK_SIZE (128 * 1024 * 1024)

typedef enum {
	ActionTypeAddFile,
	ActionTypeRemoveFile,
	ActionTypeAddDirectory,
	ActionTypeRemoveDirectory,
	ActionTypeEditFile,
} ActionType;

typedef struct {
	int64_t time;
	ActionType actionType;
	char* path;
	char* content;
	int contentLen;
	size_t size;
	int blockSize;
} Action;

// returns non-zero if the action file is rejected as a whole
int actionAdded(char* actionName, char* buf, size_t size);
void flushActionsAdded();
void actionsCleanup();
char* serializeAction(Action* action);
int64_t getNewActionTime();
void addAction(Action *newAction);

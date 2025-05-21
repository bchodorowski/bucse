#define MAX_STORAGE_NAME_LEN 64

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

void actionAdded(char* actionName, char* buf, size_t size, int moreInThisBatch);
void actionsCleanup();
char* serializeAction(Action* action);
void addAction(Action *newAction);

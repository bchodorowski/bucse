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

// returns non-zero if the action file is rejected as a whole
int actionAdded(char* actionName, char* buf, size_t size);
void flushActionsAdded();
void actionsCleanup();
char* serializeAction(Action* action);
void addAction(Action *newAction);

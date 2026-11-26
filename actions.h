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
	ActionTypeSetTimes,
} ActionType;

// What doAction() (or, for a local action, the operation) did with an
// action, so that undoAction() can revert it.
typedef enum {
	OutcomeNotApplied, // rejected, or nothing changed
	// addFile created the file, editFile changed it, removeFile removed it,
	// addDirectory created the directory, removeDirectory removed it
	OutcomeApplied,
	// applied to an entry that holds local data that hasn't been written
	// yet: addFile took a file created locally, removeFile kept the file
	// for its unflushed changes, addDirectory added again a directory
	// kept for local data, removeDirectory kept the directory for it
	OutcomeLocalData,
} ActionOutcome;

typedef struct {
	int64_t time;
	ActionType actionType;
	char* path;
	char* content;
	int contentLen;
	size_t size;
	int blockSize;
	// optional: the mtime and atime (in nanoseconds since the epoch) that
	// the action sets, instead of the action time (setTimes sets only the
	// ones given, the other actions use the action time for a missing one;
	// editFile doesn't change atime then)
	int hasMtime;
	int64_t mtime;
	int hasAtime;
	int64_t atime;

	// in memory only, never in the action file: what was done with the
	// action, and the state of the entry at path that it replaced (the
	// content is owned by an earlier action)
	ActionOutcome outcome;
	int64_t prevAtime;
	int64_t prevMtime;
	char* prevContent;
	int prevContentLen;
	size_t prevSize;
	int prevBlockSize;
	int prevPendingCreate; // of a directory that removeDirectory removed
} Action;

// returns non-zero if the action file is rejected as a whole
int actionAdded(char* actionName, char* buf, size_t size);
void flushActionsAdded();
void actionsCleanup();
char* serializeAction(Action* action);
int64_t getNewActionTime();
// adds a local action; call it before the operation changes the tree
void addAction(Action *newAction);

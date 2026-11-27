typedef enum {
	DirtyFlagNotDirty = 0,
	DirtyFlagPendingCreate = 1,
	DirtyFlagPendingWrite = 2,
	DirtyFlagPendingCreateAndWrite = 3,
	DirtyFlagPendingTrunc = 4,
} DirtyFlags;

typedef struct {
	char *buf;
	size_t size;
	size_t capacity; // allocated size of buf, >= size
	off_t offset;
} PendingWrite;

typedef struct _FilesystemDir
{
	const char* name; // pointer to memory that is managed by actions
	int64_t atime; // in nanoseconds since the epoch
	int64_t mtime; // in nanoseconds since the epoch
	DynArray files;
	DynArray dirs;
	struct _FilesystemDir* parentDir;
	// another client removed this directory, but it is kept here because
	// it holds data that is not in the repository yet (unflushed files);
	// an addDirectory action has to be written before anything inside it
	int pendingCreate;
} FilesystemDir;

typedef struct
{
	const char* name; // pointer to memory that is managed by actions
	int64_t atime; // in nanoseconds since the epoch
	int64_t mtime; // in nanoseconds since the epoch
	char* content; // pointer to memory that is managed by actions
	int contentLen;
	size_t size;
	int blockSize;
	DirtyFlags dirtyFlags;
	DynArray pendingWrites;
	size_t pendingWritesSize; // the sum of pendingWrites' sizes
	// the time of the last pending write (in nanoseconds since the epoch),
	// shown as the mtime while DirtyFlagPendingWrite is set; in memory
	// only, the flush sets the mtime to the action's time
	int64_t pendingWriteMtime;
	FilesystemDir* parentDir;
	size_t truncSize;
} FilesystemFile;

extern FilesystemDir* root;

FilesystemFile* findFile(FilesystemDir* dir, const char* fileName);
FilesystemDir* findDir(FilesystemDir* dir, const char* dirName);
FilesystemDir* findContainingDir(DynArray *pathArray);
FilesystemDir* findDirByPath(DynArray *pathArray);

// path_split splits path into directories and file names. It returns a const pointer
// to the part of path string that corresponds to the file name
const char* path_split(const char* path, DynArray *result);

void path_free(DynArray *pathArray);
char* path_getFilename(DynArray *pathArray);
void path_debugPrint(DynArray *pathArray);

char* getFullFilePath(FilesystemFile* file);
char* getFullDirPath(FilesystemDir* dir);

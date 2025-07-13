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
	off_t offset;
} PendingWrite;

typedef struct _FilesystemDir
{
	const char* name; // pointer to memory that is managed by actions
	int64_t atime;
	int64_t mtime;
	DynArray files;
	DynArray dirs;
	struct _FilesystemDir* parentDir;
} FilesystemDir;

typedef struct
{
	const char* name; // pointer to memory that is managed by actions
	int64_t atime;
	int64_t mtime;
	char* content; // pointer to memory that is managed by actions
	int contentLen;
	size_t size;
	int blockSize;
	DirtyFlags dirtyFlags;
	DynArray pendingWrites;
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

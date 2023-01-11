#define FUSE_USE_VERSION 34

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <fuse_lowlevel.h>
#include <fuse.h>

#include <json.h>

#include <pthread.h>

#include "destinations/dest.h"
#include "encryption/encr.h"

#define PACKAGE_VERSION "current"

typedef struct {
	void** objects;
	int len;
	int size;
} DynArray;

static int64_t getCurrentTime()
{
	// TODO
	return 0;
}

static int addToDynArray(DynArray *dynArray, void* newObject)
{
	int oldDynArraySize = dynArray->size;

	if (dynArray->len == dynArray->size) {
		int newSize;

		if (dynArray->size == 0) {
			newSize = 16;
		} else {
			newSize = oldDynArraySize * 2;
		}

		void* newObjects = malloc(newSize * sizeof(void*));
		if (newObjects == NULL) {
			fprintf(stderr, "addToDynArray: malloc(): %s\n", strerror(errno));
			return 1;
		}
		if (dynArray->objects != NULL) {
			memcpy(newObjects, dynArray->objects, oldDynArraySize * sizeof(void*));
			free(dynArray->objects);
		}
		dynArray->objects = newObjects;
		dynArray->size = newSize;
	}

	dynArray->objects[dynArray->len] = newObject;
	dynArray->len++;

	return 0;
}

static void freeDynArray(DynArray *dynArray)
{
	if (dynArray->objects != NULL) {
		free(dynArray->objects);
	}
	dynArray->objects = NULL;
	dynArray->len = dynArray->size = 0;
}

typedef enum {
	ActionTypeAddFile,
	ActionTypeRemoveFile,
	ActionTypeAddDirectory,
	ActionTypeRemoveDirectory
} ActionType;

typedef struct {
	int64_t time;
	ActionType actionType;
	char* path;
	char* content;
	int contentLen;
	int size;
	int blockSize;
} Action;

static void printActions(DynArray* array)
{
	for (int i=0; i<array->len; i++) {
		Action* action = ((Action*)array->objects[i]);
		printf("- %lld\n", action->time);
	}
}

static int compareActionsByTime(const void* a1, const void* a2)
{
	if ((*(const Action**)a1)->time > (*(const Action**)a2)->time) {
		return 1;
	} else if ((*(const Action**)a1)->time < (*(const Action**)a2)->time) {
		return -1;
	} else {
		return 0;
	}
}

void freeAction(Action* action)
{
	if (action == NULL) {
		return;
	}

	if (action->path != NULL) {
		free(action->path);
	}

	if (action->content != NULL) {
		free(action->content);
	}

	free(action);
}

#define MAX_STORAGE_NAME_LEN 64

// -- filesystem

typedef enum {
	DirtyFlagNotDirty = 0,
	DirtyFlagPendingCreate = 1,
	DirtyFlagPendingWrite = 2,
	DirtyFlagPendingCreateAndWrite = 3,
} DirtyFlags;

typedef struct {
	char *buf;
	size_t size;
	off_t offset;
} PendingWrite;

typedef struct
{
	const char* name;
	int64_t time;
	char* content;
	int contentLen;
	int size;
	int blockSize;
	DirtyFlags dirtyFlags;
	DynArray pendingWrites;
} FilesystemFile;

typedef struct
{
	char* name;
	int64_t time;
	DynArray files;
	DynArray dirs;
} FilesystemDir;

static FilesystemDir* root;

static void recursivelyFreeFilesystem(FilesystemDir* dir) {
	for (int i=0; i<dir->dirs.len; i++) {
		recursivelyFreeFilesystem(dir->dirs.objects[i]);
	}
	for (int i=0; i<dir->files.len; i++) {
		free(dir->files.objects[i]);
	}
	
	freeDynArray(&dir->dirs);
	freeDynArray(&dir->files);
	free(dir);
}

// path_split splits path into directories and file names. It returns a const pointer
// to the part of path string that corresponds to the file name
static const char* path_split(const char* path, DynArray *result)
{
	char* pathCopy = strdup(path);
	if (pathCopy == NULL) {
		fprintf(stderr, "splitPath: strdup(): %s\n", strerror(errno));
		return NULL;
	}

	char* last = pathCopy;
	int len = strlen(pathCopy);
	for (int i=0; i<len; i++) {
		if (pathCopy[i] == '/') {
			pathCopy[i] = 0;
			addToDynArray(result, last);
			last = pathCopy + i + 1;
		}
	}
	addToDynArray(result, last);
	return path + (last - pathCopy);
}

static void path_free(DynArray *pathArray)
{
	if (pathArray->len > 0 && pathArray->objects[0] != NULL) {
		free(pathArray->objects[0]);
	}
	freeDynArray(pathArray);
}

static char* path_getFilename(DynArray *pathArray)
{
	return pathArray->objects[pathArray->len-1];
}

static void path_debugPrint(DynArray *pathArray)
{
	fprintf(stderr, "DEBUG: print debug path:\n");
	for (int i=0; i<pathArray->len; i++) {
		fprintf(stderr, "\t%d: %s\n", i, pathArray->objects[i]);
	}
}

static FilesystemFile* findFile(FilesystemDir* dir, const char* fileName)
{
	for (int i=0; i<dir->files.len; i++) {
		FilesystemFile* f = dir->files.objects[i];
		if (strcmp(f->name, fileName) == 0) {
			return f;
		}
	}

	return NULL;
}

static FilesystemDir* findDir(FilesystemDir* dir, const char* dirName)
{
	for (int i=0; i<dir->dirs.len; i++) {
		FilesystemDir* d = dir->dirs.objects[i];
		if (strcmp(d->name, dirName) == 0) {
			return d;
		}
	}
}

static FilesystemDir* findContainingDir(DynArray *pathArray)
{
	FilesystemDir* current = root;
	for (int i=0; i<pathArray->len-1; i++) {
		char found = 0;

		current = findDir(current, pathArray->objects[i]);
		if (current == NULL) {
			return NULL;
		}
	}
	return current;
}

static FilesystemDir* findDirByPath(DynArray *pathArray)
{
	FilesystemDir* current = root;
	for (int i=0; i<pathArray->len; i++) {
		char found = 0;

		current = findDir(current, pathArray->objects[i]);
		if (current == NULL) {
			return NULL;
		}
	}
	return current;
}

static int doAction(Action* action)
{
	printf("DEBUG: do action\n");

	if (action->actionType == ActionTypeAddFile) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(action->path, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "doAction: path_split() failed\n");
			return 1;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "doAction: path not found when adding file %s\n", action->path);
			return 2;
		}

		FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
		if (newFile == NULL) {
			fprintf(stderr, "doAction: malloc(): %s\n", strerror(errno));
			return 3;
		}

		newFile->name = fileName;
		newFile->time = action->time;
		newFile->content = action->content;
		newFile->contentLen = action->contentLen;
		newFile->size = action->size;
		newFile->blockSize = action->blockSize;
		newFile->dirtyFlags = 0;
		memset(&newFile->pendingWrites, 0, sizeof(DynArray));

		addToDynArray(&containingDir->files, newFile);
		return 0;
	}

	return -1;
}

static int undoAction(Action* action)
{
	// TODO
	printf("DEBUG: undo action\n");
}

// -- end of filesystem

// TODO: mutex guarding those?
static DynArray actions;
static DynArray actionsPending;

// parses actions json document and appends actionsPending array with the results
static void parseAction(char* buf, size_t size)
{
	json_tokener* tokener = json_tokener_new();
	json_object* obj = json_tokener_parse_ex(tokener, buf, size);
	if (obj == NULL)
	{
		fprintf(stderr, "actionAdded: json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		json_tokener_free(tokener);
		return;
	}
	json_tokener_free(tokener);

	if (json_object_get_type(obj) != json_type_array) {
		fprintf(stderr, "actionAdded: document is not an array\n");
		json_object_put(obj);
		return;
	}

	// This json document is an array of action objects. Parse each of them and add to actionsPending.
	for (size_t i=0; i<json_object_array_length(obj); i++) {
		json_object* actionObj = json_object_array_get_idx(obj, i);

		if (json_object_get_type(actionObj) != json_type_object) {
			fprintf(stderr, "actionAdded: array element is not an object\n");
			continue;
		}

		// parse time
		json_object* timeField;
		if (json_object_object_get_ex(actionObj, "time", &timeField) == 0) {
			fprintf(stderr, "actionAdded: action object doesn't have 'time' field\n");
			continue;
		}
		if (json_object_get_type(timeField) != json_type_int) {
			fprintf(stderr, "actionAdded: 'time' field is not an integer\n");
			continue;
		}
		int64_t time = json_object_get_int64(timeField);

		// parse size
		json_object* sizeField;
		int64_t size = 0;
		if (json_object_object_get_ex(actionObj, "size", &sizeField) != 0) {
			if (json_object_get_type(sizeField) != json_type_int) {
				fprintf(stderr, "actionAdded: 'size' field is not an integer\n");
				continue;
			}
			size = json_object_get_int64(sizeField);
		}

		// parse blockSize
		json_object* blockSizeField;
		int64_t blockSize = 0;
		if (json_object_object_get_ex(actionObj, "blockSize", &blockSizeField) != 0) {
			if (json_object_get_type(blockSizeField) != json_type_int) {
				fprintf(stderr, "actionAdded: 'blockSize' field is not an integer\n");
				continue;
			}
			blockSize = json_object_get_int64(blockSizeField);
		}

		// parse action
		json_object* actionTypeField;
		if (json_object_object_get_ex(actionObj, "action", &actionTypeField) == 0) {
			fprintf(stderr, "actionAdded: action object doesn't have 'action' field\n");
			continue;
		}
		if (json_object_get_type(actionTypeField) != json_type_string) {
			fprintf(stderr, "actionAdded: 'action' field is not a string\n");
			continue;
		}
		const char* actionTypeStr = json_object_get_string(actionTypeField);
		ActionType actionType;
		if (strcmp(actionTypeStr, "addFile") == 0) {
			actionType = ActionTypeAddFile;
		} else if (strcmp(actionTypeStr, "removeFile") == 0) {
			actionType = ActionTypeRemoveFile;
		} else if (strcmp(actionTypeStr, "addDirectory") == 0) {
			actionType = ActionTypeAddDirectory;
		} else if (strcmp(actionTypeStr, "removeDirectory") == 0) {
			actionType = ActionTypeRemoveDirectory;
		} else {
			fprintf(stderr, "actionAdded: unknown action\n");
			continue;
		}

		// parse path
		json_object* pathField;
		if (json_object_object_get_ex(actionObj, "path", &pathField) == 0) {
			fprintf(stderr, "actionAdded: action object doesn't have 'path' field\n");
			continue;
		}
		if (json_object_get_type(pathField) != json_type_string) {
			fprintf(stderr, "actionAdded: 'path' field is not a string\n");
			continue;
		}
		const char* path = json_object_get_string(pathField);

		// parse content
		json_object* contentField;
		if (json_object_object_get_ex(actionObj, "content", &contentField) == 0) {
			fprintf(stderr, "actionAdded: action object doesn't have 'content' field\n");
			continue;
		}
		if (json_object_get_type(contentField) != json_type_array) {
			fprintf(stderr, "actionAdded: 'content' field is not an array\n");
			continue;
		}
		size_t contentLen = json_object_array_length(contentField);
		char* content = malloc(contentLen * MAX_STORAGE_NAME_LEN);
		if (content == NULL) {
			fprintf(stderr, "actionAdded: malloc(): %s\n", strerror(errno));
			continue;
		}
		size_t j;
		for (j=0; j<contentLen; j++) {
			json_object* contentItemField = json_object_array_get_idx(contentField, j);

			if (json_object_get_type(contentItemField) != json_type_string) {
				fprintf(stderr, "actionAdded: 'content' contents is not a string\n");
				break;
			}
			const char* contentItemStr = json_object_get_string(contentItemField);
			snprintf(content + (MAX_STORAGE_NAME_LEN * j), MAX_STORAGE_NAME_LEN, "%s", contentItemStr);
		}
		if (j != contentLen) {
			free(content);
			continue;
		}

		// create new action object
		Action* newAction = malloc(sizeof(Action));
		if (newAction == NULL) {
			fprintf(stderr, "actionAdded: malloc(): %s\n", strerror(errno));
			free(content);
			continue;
		}
		newAction->time = time;
		newAction->actionType = actionType;
		newAction->path = malloc(strlen(path) + 1);
		if (newAction->path == NULL) {
			fprintf(stderr, "actionAdded: malloc(): %s\n", strerror(errno));
			free(content);
			free(newAction);
			continue;
		}
		memcpy(newAction->path, path, strlen(path) + 1);
		newAction->content = content;
		newAction->contentLen = contentLen;
		newAction->size = size;
		newAction->blockSize = blockSize;

		addToDynArray(&actionsPending, newAction);
	}

	json_object_put(obj);
}

void actionAdded(char* actionName, char* buf, size_t size, int moreInThisBatch)
{
	//printf("%s\n  %s\n  %d\n  %d\n", actionName, buf, size, moreInThisBatch);

	parseAction(buf, size);

	// early out if there is more data incoming
	if (moreInThisBatch > 0) {
		return;
	}

	// early out if there is no pending action
	if (actionsPending.len == 0) {
		return;
	}

	// prepare actionsPending

	qsort(actionsPending.objects, actionsPending.len, sizeof(void*), compareActionsByTime);
	printActions(&actionsPending);

	// handle out of order actions
	int outOfOrderActions = 0;
	while (actions.len > 0 && actionsPending.len > 0
		&& ((Action*)actions.objects[actions.len - 1])->time >= // time of last action in actions
			((Action*)actionsPending.objects[0])->time // time of first action in actionsPending
	      ) {
		printf("DEBUG: undoing action due to out of order action!\n");
		// move last action to actionsPending
		addToDynArray(&actionsPending, actions.objects[actions.len - 1]);
		actions.len--;

		undoAction(actions.objects[actions.len]);

		outOfOrderActions++;
	}

	if (outOfOrderActions > 0) {
		qsort(actionsPending.objects, actionsPending.len, sizeof(void*), compareActionsByTime);
	}

	// move actions from actinsPending to actions, acting on them
	for (int i=0; i<actionsPending.len; i++) {
		addToDynArray(&actions, actionsPending.objects[i]);
		doAction(actionsPending.objects[i]);
	}
	freeDynArray(&actionsPending);
}

static char TESTCONTENT[] = "Wololo\n";
static char TESTFILENAME[] = "test";

extern Destination destinationLocal;
extern Encryption encryptionNone;

static Destination *destination;
static Encryption *encryption;
static pthread_t tickThread;

static pthread_mutex_t shutdownMutex;
static int shutdownTicking = 0;


static int bucse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	(void) fi;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_getattr: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			stbuf->st_mode = S_IFREG | 0444;
			stbuf->st_nlink = 1;
			stbuf->st_size = file->size;
		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 1;
			} else {
				return -ENOENT;
			}
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int bucse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	(void) offset;
	(void) fi;
	(void) flags;

	FilesystemDir *dir;

	if (strcmp(path, "/") == 0) {
		dir = root;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_readdir: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		dir = findDirByPath(&pathArray);
		path_free(&pathArray);
	} else {
		return -ENOENT;
	}

	if (dir == NULL) {
		return -ENOENT;
	}

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	for (int i=0; i<dir->dirs.len; i++) {
		FilesystemDir* d = dir->dirs.objects[i];
		filler(buf, d->name, NULL, 0, 0);
	}

	for (int i=0; i<dir->files.len; i++) {
		FilesystemFile* f = dir->files.objects[i];
		filler(buf, f->name, NULL, 0, 0);
	}
	return 0;
}

static int bucse_open(const char *path, struct fuse_file_info *fi)
{
	fprintf(stderr, "DEBUG: open %s, access mode %d\n", path, (fi->flags & O_ACCMODE));

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_open: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_open: path not found when opening file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			return 0;
		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
	} else {
		return -ENOENT;
	}
}

static int bucse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	fprintf(stderr, "DEBUG: create %s, mode %d, access mode %d\n", path, mode, (fi->flags & O_ACCMODE));

	FilesystemDir *containingDir = NULL;
	const char *fileName = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_create: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);
		if (containingDir == NULL) {
			fprintf(stderr, "bucse_create: path not found when creating file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			return -EEXIST;
		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EEXIST;
			} else {
				// continue below
			}
		}
	} else {
		return -ENOENT;
	}

	FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
	if (newFile == NULL) {
		fprintf(stderr, "bucse_create: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}

	newFile->name = strdup(fileName); // TODO: free when clearing the dirty flag
	if (newFile->name == NULL) {
		fprintf(stderr, "splitPath: strdup(): %s\n", strerror(errno));
		free(newFile);
		return -ENOMEM;
	}
	newFile->time = getCurrentTime();
	newFile->content = NULL;
	newFile->contentLen = 0;
	newFile->size = 0;
	newFile->blockSize = 0;
	newFile->dirtyFlags = DirtyFlagPendingCreate;
	memset(&newFile->pendingWrites, 0, sizeof(DynArray));

	addToDynArray(&containingDir->files, newFile);
	return 0;
}

static int bucse_release(const char *path, struct fuse_file_info *fi)
{
	fprintf(stderr, "DEBUG: release %s, access mode %d\n", path, (fi->flags & O_ACCMODE));

	return 0;
}

typedef struct {
	const char* block;
	off_t offset;
	size_t len;
} BlockOffsetLen;

// use offset and size to determine which blocks contain the data that's needed
static int determineBlocksToRead(DynArray *blocksToRead, off_t offset, size_t size, FilesystemFile* file)
{
	if (offset > file->size) {
		return 0;
	}
	if (size > file->size - offset) {
		size = file->size - offset;
	}

	while (size > 0) {
		int blockIndex = offset / file->blockSize;
		off_t blockOffset = (offset % file->blockSize);
		size_t blockLen = file->blockSize - blockOffset;
		if (blockLen > size) {
			blockLen = size;
		}
		BlockOffsetLen *block = malloc(sizeof(BlockOffsetLen));
		if (!block) {
			fprintf(stderr, "determineBlocksToRead: malloc(): %s\n", strerror(errno));
			return 1;
		}
		block->block = file->content + MAX_STORAGE_NAME_LEN * blockIndex;
		block->offset = blockOffset;
		block->len = blockLen;
		addToDynArray(blocksToRead, block);

		offset += blockLen;
		size -= blockLen;
	}
	
	return 0;
}

// TODO: move this to Encryption[?]
static size_t getMaxEncryptedBlockSize(size_t blockSize)
{
	blockSize *= 2;
	if (blockSize < 256) {
		blockSize = 256;
	}
	return blockSize;
}

static int bucse_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	fprintf(stderr, "DEBUG: read %s, size: %u, offset: %u\n", path, size, offset);

	/*
	size_t len;
	if(strcmp(path+1, TESTFILENAME) != 0)
		return -ENOENT;

	len = strlen(TESTCONTENT);
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, TESTCONTENT + offset, size);
	} else
		size = 0;

	return size;
	*/

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_read: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_read: path not found when reading file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	// determine which blocks should be read
	DynArray blocksToRead;
	memset(&blocksToRead, 0, sizeof(DynArray));
	if (determineBlocksToRead(&blocksToRead, offset, size, file) != 0) {
		fprintf(stderr, "bucse_read: determineBlocksToRead failed\n");
		for (int i=0; i<blocksToRead.len; i++) {
			free(blocksToRead.objects[i]);
		}
		freeDynArray(&blocksToRead);
		return -ENOMEM;
	}

	size_t maxEncryptedBlockSize = getMaxEncryptedBlockSize(file->blockSize);
	char* encryptedBlockBuf = malloc(maxEncryptedBlockSize);
	if (encryptedBlockBuf == NULL) {
		fprintf(stderr, "bucse_read: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}

	size_t maxDecryptedBlockSize = file->blockSize;
	char* decryptedBlockBuf = malloc(maxDecryptedBlockSize);
	if (decryptedBlockBuf == NULL) {
		fprintf(stderr, "bucse_read: malloc(): %s\n", strerror(errno));
		free(encryptedBlockBuf);
		return -ENOMEM;
	}

	size_t copiedBytes = 0;
	int ioerror = 0;
	for (int i=0; i<blocksToRead.len; i++) {
		BlockOffsetLen* block = blocksToRead.objects[i];
		size_t encryptedBlockBufSize = maxEncryptedBlockSize;
		int res = destination->getStorageFile(block->block, encryptedBlockBuf, &encryptedBlockBufSize);
		if (res != 0) {
			fprintf(stderr, "bucse_read: getStorageFile failed for %s: %d\n",
				block->block, res);
			ioerror = 1;
			break;
		}

		size_t decryptedBlockBufSize = maxDecryptedBlockSize;
		res = encryption->decrypt(encryptedBlockBuf, encryptedBlockBufSize,
			decryptedBlockBuf, &decryptedBlockBufSize,
			""); // TODO: manage encryption key
		if (res != 0) {
			fprintf(stderr, "bucse_read: decrypt failed: %d\n", res);
			ioerror = 1;
			break;
		}
		if (decryptedBlockBufSize != maxDecryptedBlockSize) {
			fprintf(stderr, "bucse_read: expected decrypted block size %d, got %d\n",
				maxDecryptedBlockSize, decryptedBlockBufSize);
			ioerror = 1;
			break;
		}

		memcpy(buf + copiedBytes, decryptedBlockBuf + block->offset, block->len);
		copiedBytes += block->len;
	}

	free(encryptedBlockBuf);
	free(decryptedBlockBuf);

	for (int i=0; i<blocksToRead.len; i++) {
		free(blocksToRead.objects[i]);
	}
	freeDynArray(&blocksToRead);

	if (ioerror) {
		return -EIO;
	}

	return copiedBytes;
}

static int bucse_write(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	fprintf(stderr, "DEBUG: write %s, size: %u, offset: %u\n", path, size, offset);

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_write: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_write: path not found when reading file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	file->dirtyFlags |= DirtyFlagPendingWrite;
	PendingWrite* newPendingWrite = malloc(sizeof(PendingWrite));
	if (newPendingWrite == NULL) {
		fprintf(stderr, "bucse_write: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newPendingWrite->size = size;
	newPendingWrite->buf = malloc(size);
	if (newPendingWrite->buf == NULL) {
		fprintf(stderr, "bucse_write: malloc(): %s\n", strerror(errno));
		free(newPendingWrite);
		return -ENOMEM;
	}
	memcpy(newPendingWrite->buf, buf, size);
	newPendingWrite->offset = offset;
	addToDynArray(&file->pendingWrites, newPendingWrite);

	return size;
}

struct fuse_operations bucse_oper = {
	.getattr = bucse_getattr,
	.open = bucse_open,
	.create = bucse_create,
	.release = bucse_release,
	.read = bucse_read,
	.readdir = bucse_readdir,
	.write = bucse_write,
};

struct bucse_config {
	char *repository;
};

enum {
	KEY_HELP,
	KEY_VERSION,
};

#define BUCSE_OPT(t, p, v) { t, offsetof(struct bucse_config, p), v }

static struct fuse_opt bucse_opts[] = {
	BUCSE_OPT("repository=%s", repository, 0),
	BUCSE_OPT("-r %s", repository, 0),

	FUSE_OPT_KEY("-V",             KEY_VERSION),
	FUSE_OPT_KEY("--version",      KEY_VERSION),
	FUSE_OPT_KEY("-h",             KEY_HELP),
	FUSE_OPT_KEY("--help",         KEY_HELP),
	FUSE_OPT_END
};

static int bucse_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	switch (key) {
	case KEY_HELP:
		fuse_opt_add_arg(outargs, "-h");
		fuse_main(outargs->argc, outargs->argv, &bucse_oper, NULL);
		fprintf(stderr,
				"\n"
				"bucse options:\n"
				"    -o repository=STRING   TODO\n"
				"    -r STRING              same as '-orepository=STRING'\n"
				, outargs->argv[0]);
		exit(1);

	case KEY_VERSION:
		fprintf(stderr, "bucse version %s\n", PACKAGE_VERSION);
		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &bucse_oper, NULL);
		exit(0);
	}
	return 1;
}

void* tickThreadFunc(void* param)
{
	printf("DEBUG: Hello from tickThreadFunc\n");
	for (;;)
	{
		pthread_mutex_lock(&shutdownMutex);
		if (shutdownTicking)
		{
			pthread_mutex_unlock(&shutdownMutex);
			break;
		}
		pthread_mutex_unlock(&shutdownMutex);

		if (destination->tick() != 0)
		{
			break;
		}
		sleep(1);
	}
	return 0;
}

int bucse_fuse_main(int argc, char *argv[], const struct fuse_operations *op,
		size_t op_size, void *user_data)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse *fuse;
	struct fuse_cmdline_opts opts;
	int res;
	struct fuse_loop_config config;

	if (fuse_parse_cmdline(&args, &opts) != 0)
		return 1;

	if (opts.show_version) {
		printf("FUSE library version %s\n", PACKAGE_VERSION);
		fuse_lowlevel_version();
		res = 0;
		goto out1;
	}

	if (opts.show_help) {
		if(args.argv[0][0] != '\0')
			printf("usage: %s [options] <mountpoint>\n\n",
					args.argv[0]);
		printf("FUSE options:\n");
		fuse_cmdline_help();
		fuse_lib_help(&args);
		res = 0;
		goto out1;
	}

	if (!opts.show_help &&
			!opts.mountpoint) {
		fuse_log(FUSE_LOG_ERR, "error: no mountpoint specified\n");
		res = 2;
		goto out1;
	}

	fuse = fuse_new(&args, op, op_size, user_data);
	if (fuse == NULL) {
		res = 3;
		goto out1;
	}

	if (fuse_mount(fuse,opts.mountpoint) != 0) {
		res = 4;
		goto out2;
	}

	// TODO: switch daemonize with an argument[?]
	/*
	if (fuse_daemonize(opts.foreground) != 0) {
		res = 5;
		goto out3;
	}
	*/

	// initialize destination thread
	if (destination->isTickable())
	{
		int ret = pthread_create(&tickThread, NULL, tickThreadFunc, NULL);
	}

	struct fuse_session *se = fuse_get_session(fuse);
	if (fuse_set_signal_handlers(se) != 0) {
		res = 6;
		goto out3;
	}

	if (opts.singlethread)
		res = fuse_loop(fuse);
	else {
		config.clone_fd = opts.clone_fd;
		config.max_idle_threads = opts.max_idle_threads;
		res = fuse_session_loop_mt(se, &config);
	}
	if (res)
		res = 8;

	fuse_remove_signal_handlers(se);
out3:
	fuse_unmount(fuse);
out2:
	fuse_destroy(fuse);
out1:
	free(opts.mountpoint);
	fuse_opt_free_args(&args);
	return res;
}

#define MAX_REPOSITORY_JSON_LEN (1024 * 1024)

int main(int argc, char** argv)
{
	root = malloc(sizeof(FilesystemDir));
	if (root == NULL) {
		fprintf(stderr, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	memset(root, 0, sizeof(FilesystemDir));

	destination = &destinationLocal;

	destination->setCallbackActionAdded(&actionAdded);

	encryption = &encryptionNone;


	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct bucse_config conf;

	memset(&conf, 0, sizeof(conf));

	fuse_opt_parse(&args, &conf, bucse_opts, bucse_opt_proc);

	printf("Repository: %s\n", conf.repository);
	int err = destination->init(conf.repository);
	if (err != 0)
	{
		fprintf(stderr, "destination->init(): %d\n", err);
		return 2;
	}

	char* repositoryJsonFileContents = malloc(MAX_REPOSITORY_JSON_LEN);
	if (repositoryJsonFileContents == NULL)
	{
		fprintf(stderr, "malloc(): %s\n", strerror(errno));
		return 3;
	}
	size_t repositoryJsonFileLen = MAX_REPOSITORY_JSON_LEN;

	err = destination->getRepositoryFile(repositoryJsonFileContents, &repositoryJsonFileLen);
	if (err != 0)
	{
		fprintf(stderr, "destination->getRepositoryFile(): %d\n", err);

		free(repositoryJsonFileContents);
		return 4;
	}

	json_tokener* tokener = json_tokener_new();
	json_object* repositoryJson = json_tokener_parse_ex(tokener, repositoryJsonFileContents, repositoryJsonFileLen);

	free(repositoryJsonFileContents);
	json_tokener_free(tokener);

	if (repositoryJson == NULL)
	{
		fprintf(stderr, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		return 5;
	}

	printf("DEBUG: type: %s\n", json_type_to_name(
				json_object_get_type(repositoryJson)));

	json_object* encryptionField;
	if (json_object_object_get_ex(repositoryJson, "encryption", &encryptionField) == 0)
	{
		fprintf(stderr, "repository object doesn't have 'encryption' field\n");
		json_object_put(repositoryJson);
		return 6;
	}

	if (json_object_get_type(encryptionField) != json_type_string)
	{
		fprintf(stderr, "'encryption' field is not a string\n");
		json_object_put(repositoryJson);
		return 7;
	}

	const char* encryptionFieldStr = json_object_get_string(encryptionField);

	printf("DEBUG: enc: %s\n", encryptionFieldStr);
	if (strcmp(encryptionFieldStr, "none") == 0)
	{
		printf("DEBUG: encryption none\n");
	}
	else
	{
		fprintf(stderr, "Unsupported encryption: %s\n", encryptionFieldStr);
		json_object_put(repositoryJson);
		return 8;
	}

	json_object_put(repositoryJson);

	int fuse_stat;
	fuse_stat = bucse_fuse_main(args.argc, args.argv, &bucse_oper, sizeof(bucse_oper), NULL);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
	pthread_mutex_lock(&shutdownMutex);
	shutdownTicking = 1;
	pthread_mutex_unlock(&shutdownMutex);

	int ret = pthread_join(tickThread, NULL);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_join: %d\n", ret);
	}
	destination->shutdown();

	// free filesystem
	recursivelyFreeFilesystem(root);

	// free actions
	for (int i=0; i<actions.len; i++) {
		freeAction(actions.objects[i]);
	}
	freeDynArray(&actions);

	// free actionsPending
	for (int i=0; i<actionsPending.len; i++) {
		freeAction(actionsPending.objects[i]);
	}
	freeDynArray(&actionsPending);

	return fuse_stat;
}

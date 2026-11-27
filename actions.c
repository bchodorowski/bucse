#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>

#include <json.h>

#include "dynarray.h"
#include "filesystem.h"
#include "log.h"
#include "time.h"

#include "actions.h"

// TODO: unused fields should not be serialized nor parsed
// -- e.g. content, size, blockSize for removeFile action

static void printActions(DynArray* array)
{
	logPrintf(LOG_VERBOSE_DEBUG, "printActions():\n");
	for (int i=0; i<array->len; i++) {
		Action* action = ((Action*)array->objects[i]);
		logPrintf(LOG_VERBOSE_DEBUG, "- %" PRId64 "\n", action->time);
	}
}

// Orders actions by time, and actions with the same time (e.g. the two
// actions of a rename) by all their other fields. So the order is the same
// on every client, and only identical actions compare equal, which
// deduplication in flushActionsAdded() relies on (identical actions are
// adjacent once sorted).
static int compareActions(const Action* a1, const Action* a2)
{
	if (a1->time != a2->time) {
		return a1->time > a2->time ? 1 : -1;
	}
	if (a1->actionType != a2->actionType) {
		return a1->actionType > a2->actionType ? 1 : -1;
	}
	int c = strcmp(a1->path, a2->path);
	if (c != 0) {
		return c;
	}
	if (a1->contentLen != a2->contentLen) {
		return a1->contentLen > a2->contentLen ? 1 : -1;
	}
	// each content entry is a string in a MAX_STORAGE_NAME_LEN slot, the
	// bytes after its end aren't initialised
	for (int i=0; i<a1->contentLen; i++) {
		c = strncmp(a1->content + i*MAX_STORAGE_NAME_LEN,
			a2->content + i*MAX_STORAGE_NAME_LEN, MAX_STORAGE_NAME_LEN);
		if (c != 0) {
			return c;
		}
	}
	if (a1->size != a2->size) {
		return a1->size > a2->size ? 1 : -1;
	}
	if (a1->blockSize != a2->blockSize) {
		return a1->blockSize > a2->blockSize ? 1 : -1;
	}
	if (a1->hasMtime != a2->hasMtime) {
		return a1->hasMtime > a2->hasMtime ? 1 : -1;
	}
	if (a1->hasMtime && a1->mtime != a2->mtime) {
		return a1->mtime > a2->mtime ? 1 : -1;
	}
	if (a1->hasAtime != a2->hasAtime) {
		return a1->hasAtime > a2->hasAtime ? 1 : -1;
	}
	if (a1->hasAtime && a1->atime != a2->atime) {
		return a1->atime > a2->atime ? 1 : -1;
	}
	return 0;
}

static int compareActionPtrs(const void* a1, const void* a2)
{
	return compareActions(*(const Action**)a1, *(const Action**)a2);
}

static void freeAction(Action* action)
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

// returns non-zero if dir holds a file or directory that is in the
// repository, i.e. not only local data that hasn't been written yet
static int hasEntriesInRepository(FilesystemDir* dir)
{
	for (int i=0; i<dir->files.len; i++) {
		FilesystemFile* f = dir->files.objects[i];
		if (!(f->dirtyFlags & DirtyFlagPendingCreate)) {
			return 1;
		}
	}
	for (int i=0; i<dir->dirs.len; i++) {
		FilesystemDir* d = dir->dirs.objects[i];
		if (!d->pendingCreate) {
			return 1;
		}
	}
	return 0;
}

// names of the directories renamed by renameLocalDir() or created by
// restoreStashedFiles(), freed in actionsCleanup() (a directory doesn't own
// its name)
static DynArray localDirNames;

// Returns a new name for a local entry called name in dir that clashes with
// an entry of the other type added by another client: "name.conflict", or
// "name.conflictN" if that is taken too. The result is malloc()ed.
static char* conflictName(FilesystemDir* dir, const char* name)
{
	size_t len = strlen(name) + sizeof(".conflict") + 20;
	char* result = malloc(len);
	if (result == NULL) {
		logPrintf(LOG_ERROR, "conflictName: malloc(): %s\n", strerror(errno));
		return NULL;
	}
	snprintf(result, len, "%s.conflict", name);
	for (int i=2; findFile(dir, result) != NULL || findDir(dir, result) != NULL; i++) {
		snprintf(result, len, "%s.conflict%d", name, i);
	}
	return result;
}

// Renames a directory that holds only local data (dir->pendingCreate), whose
// name is taken by an entry of the other type in the repository.
static int renameLocalDir(FilesystemDir* containingDir, FilesystemDir* dir, const char* name)
{
	char* newName = conflictName(containingDir, name);
	if (newName == NULL) {
		return 1;
	}
	if (addToDynArray(&localDirNames, newName) != 0) {
		free(newName);
		return 2;
	}
	logPrintf(LOG_WARNING, "%s added by another client, renamed the local unwritten directory to %s\n", name, newName);
	dir->name = newName;
	return 0;
}

// Returns the directory that contains path, and in *name the last part of
// path (a pointer into it); NULL if a directory on the way doesn't exist.
static FilesystemDir* findParent(const char* path, const char** name)
{
	DynArray pathArray;
	memset(&pathArray, 0, sizeof(DynArray));
	*name = path_split(path, &pathArray);
	if (*name == NULL) {
		logPrintf(LOG_ERROR, "findParent: path_split() failed\n");
		return NULL;
	}
	FilesystemDir* dir = findContainingDir(&pathArray);
	path_free(&pathArray);
	return dir;
}

// Keeps a file that is in the repository as a local one, created but not
// flushed yet: its flush adds it again. A no-op for such a file.
static int markFilePendingCreate(FilesystemFile* file)
{
	if (file->dirtyFlags & DirtyFlagPendingCreate) {
		return 0;
	}
	// the name of a pending create is owned by the file (see open.c)
	char* name = strdup(file->name);
	if (name == NULL) {
		logPrintf(LOG_ERROR, "markFilePendingCreate: strdup(): %s\n", strerror(errno));
		return 1;
	}
	file->name = name;
	file->dirtyFlags |= DirtyFlagPendingCreate;
	return 0;
}

// The mtime and atime that an action that adds or edits an entry gives it:
// the ones it carries, or its time.
static int64_t actionMtime(const Action* action)
{
	return action->hasMtime ? action->mtime : microsecondsToNanoseconds(action->time);
}

static int64_t actionAtime(const Action* action)
{
	return action->hasAtime ? action->atime : microsecondsToNanoseconds(action->time);
}

// Finds the times of the entry at path (the root directory for ""), for a
// setTimes action. Fails if there is no such entry in the repository: a
// file or directory that holds only local data isn't there on other clients.
static int findTimes(const char* path, int64_t** atime, int64_t** mtime)
{
	if (path[0] == '\0') {
		*atime = &root->atime;
		*mtime = &root->mtime;
		return 0;
	}
	const char* name;
	FilesystemDir* containingDir = findParent(path, &name);
	if (containingDir == NULL || containingDir->pendingCreate) {
		return 1;
	}
	FilesystemFile* file = findFile(containingDir, name);
	if (file != NULL) {
		if (file->dirtyFlags & DirtyFlagPendingCreate) {
			return 2;
		}
		*atime = &file->atime;
		*mtime = &file->mtime;
		return 0;
	}
	FilesystemDir* dir = findDir(containingDir, name);
	if (dir != NULL) {
		if (dir->pendingCreate) {
			return 3;
		}
		*atime = &dir->atime;
		*mtime = &dir->mtime;
		return 0;
	}
	return 4;
}

// Saves the state of file that an action replaces, see Action.
static void savePrevFile(Action* action, FilesystemFile* file)
{
	action->prevAtime = file->atime;
	action->prevMtime = file->mtime;
	action->prevContent = file->content;
	action->prevContentLen = file->contentLen;
	action->prevSize = file->size;
	action->prevBlockSize = file->blockSize;
}

// Remote actions never drop local changes that haven't been flushed yet,
// see "Concurrent changes" in docs/design.md and recreatePendingDirs(). A
// directory that is kept only for local data (pendingCreate) is not in the
// repository, so an action inside it is rejected, as on every other client.
static int doAction(Action* action)
{
	logPrintf(LOG_VERBOSE_DEBUG, "do action %d %s\n", action->actionType, action->path);

	// set below when the action changes the tree
	action->outcome = OutcomeNotApplied;

	if (action->actionType == ActionTypeAddFile) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(action->path, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "doAction: path_split() failed\n");
			return 1;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL || containingDir->pendingCreate) {
			logPrintf(LOG_ERROR, "doAction: path not found when adding file %s\n", action->path);
			return 2;
		}

		FilesystemDir* dir = findDir(containingDir, fileName);
		if (dir != NULL && dir->pendingCreate) {
			// a directory kept after another client removed it, it holds
			// only local data: rename it, its addDirectory is written
			// with the new name (undoAction() doesn't rename it back)
			if (renameLocalDir(containingDir, dir, fileName) != 0) {
				return 19;
			}
		} else if (dir != NULL) {
			logPrintf(LOG_ERROR, "doAction: a directory with that name already exists, not adding file %s\n", action->path);
			return 21;
		}

		FilesystemFile* file = findFile(containingDir, fileName);
		if (file != NULL && (file->dirtyFlags & DirtyFlagPendingCreate)) {
			savePrevFile(action, file);
			action->outcome = OutcomeLocalData;
			// file->name was allocated for the pending create (see
			// open.c), point it to the action's path instead
			free((char*)file->name);
			file->name = fileName;
			file->dirtyFlags &= ~DirtyFlagPendingCreate;
			if (file->dirtyFlags == DirtyFlagNotDirty) {
				// nothing written locally, take the added file
				file->atime = actionAtime(action);
				file->mtime = actionMtime(action);
				file->content = action->content;
				file->contentLen = action->contentLen;
				file->size = action->size;
				file->blockSize = action->blockSize;
			} else {
				logPrintf(LOG_WARNING, "doAction: file %s added by another client, keeping the local unflushed one\n", action->path);
			}
			return 0;
		}
		if (file != NULL) {
			logPrintf(LOG_ERROR, "doAction: file already exists: %s\n", action->path);
			return 3;
		}

		FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
		if (newFile == NULL) {
			logPrintf(LOG_ERROR, "doAction: malloc(): %s\n", strerror(errno));
			return 4;
		}

		newFile->name = fileName;
		newFile->atime = actionAtime(action);
		newFile->mtime = actionMtime(action);
		newFile->content = action->content;
		newFile->contentLen = action->contentLen;
		newFile->size = action->size;
		newFile->blockSize = action->blockSize;
		newFile->dirtyFlags = 0;
		memset(&newFile->pendingWrites, 0, sizeof(DynArray));
		newFile->pendingWritesSize = 0;
		newFile->parentDir = containingDir;

		addToDynArray(&containingDir->files, newFile);
		action->outcome = OutcomeApplied;
		return 0;

	} else if (action->actionType == ActionTypeEditFile) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(action->path, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "doAction: path_split() failed\n");
			return 4;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL || containingDir->pendingCreate) {
			logPrintf(LOG_ERROR, "doAction: path not found when editing file %s\n", action->path);
			return 5;
		}

		FilesystemFile* file = findFile(containingDir, fileName);
		if (file == NULL) {
			logPrintf(LOG_ERROR, "doAction: file not found: %s\n", action->path);
			return 6;
		}

		// dirtyFlags and pendingWrites are kept: unflushed local changes
		// are applied on top of the new content
		if (file->dirtyFlags != DirtyFlagNotDirty) {
			logPrintf(LOG_WARNING, "doAction: file %s edited by another client, keeping the local unflushed changes\n", action->path);
		}
		savePrevFile(action, file);
		action->outcome = OutcomeApplied;
		file->mtime = actionMtime(action);
		if (action->hasAtime) {
			file->atime = action->atime;
		}
		file->content = action->content;
		file->contentLen = action->contentLen;
		file->size = action->size;
		file->blockSize = action->blockSize;
		// a pending truncate sets the size of the new content: it cuts
		// it, or extends it with zeroes (flushFile())

		return 0;

	} else if (action->actionType == ActionTypeRemoveFile) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(action->path, &pathArray);
		if (fileName == NULL) {
			logPrintf(LOG_ERROR, "doAction: path_split() failed\n");
			return 7;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL || containingDir->pendingCreate) {
			logPrintf(LOG_ERROR, "doAction: path not found when removing file %s\n", action->path);
			return 8;
		}

		FilesystemFile* file = findFile(containingDir, fileName);
		if (file == NULL) {
			logPrintf(LOG_ERROR, "doAction: file not found: %s\n", action->path);
			return 9;
		}

		if (file->dirtyFlags != DirtyFlagNotDirty) {
			// keep the file, its flush adds it again
			if (!(file->dirtyFlags & DirtyFlagPendingCreate)) {
				savePrevFile(action, file);
				if (markFilePendingCreate(file) != 0) {
					return 18;
				}
				action->outcome = OutcomeLocalData;
			}
			logPrintf(LOG_WARNING, "doAction: file %s removed by another client, keeping it for the local unflushed changes\n", action->path);
			return 0;
		}

		if (removeFromDynArrayUnordered(&containingDir->files, (void*)file) != 0) {
			logPrintf(LOG_ERROR, "doAction: removeFromDynArrayUnordered() failed\n");
			return 10;
		}
		savePrevFile(action, file);
		action->outcome = OutcomeApplied;
		free(file);
		return 0;

	} else if (action->actionType == ActionTypeAddDirectory) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *dirName = path_split(action->path, &pathArray);
		if (dirName == NULL) {
			logPrintf(LOG_ERROR, "doAction: path_split() failed\n");
			return 11;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL || containingDir->pendingCreate) {
			logPrintf(LOG_ERROR, "doAction: path not found when adding directory %s\n", action->path);
			return 12;
		}

		// a directory kept after another client removed it, now it is in
		// the repository again
		FilesystemDir* dir = findDir(containingDir, dirName);
		if (dir != NULL && dir->pendingCreate) {
			dir->pendingCreate = 0;
			action->outcome = OutcomeLocalData;
			return 0;
		}
		// already there: two clients created the same directory; keep the
		// existing one and its contents
		if (dir != NULL) {
			logPrintf(LOG_WARNING, "doAction: directory already exists: %s\n", action->path);
			return 0;
		}

		FilesystemFile* file = findFile(containingDir, dirName);
		if (file != NULL && (file->dirtyFlags & DirtyFlagPendingCreate)) {
			// a file that isn't in the repository yet: rename it, its
			// flush adds it with the new name; file->name is owned by
			// the pending create (see open.c)
			char* newName = conflictName(containingDir, dirName);
			if (newName == NULL) {
				return 22;
			}
			logPrintf(LOG_WARNING, "doAction: directory %s added by another client, renamed the local unflushed file to %s\n", action->path, newName);
			free((char*)file->name);
			file->name = newName;
		} else if (file != NULL) {
			logPrintf(LOG_ERROR, "doAction: a file with that name already exists, not adding directory %s\n", action->path);
			return 23;
		}

		FilesystemDir* newDir = malloc(sizeof(FilesystemDir));
		if (newDir == NULL) {
			logPrintf(LOG_ERROR, "doAction: malloc(): %s\n", strerror(errno));
			return 13;
		}
		memset(newDir, 0, sizeof(FilesystemDir));

		newDir->name = dirName;
		newDir->atime = actionAtime(action);
		newDir->mtime = actionMtime(action);
		newDir->parentDir = containingDir;

		addToDynArray(&containingDir->dirs, newDir);
		action->outcome = OutcomeApplied;
		return 0;

	} else if (action->actionType == ActionTypeRemoveDirectory) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *dirName = path_split(action->path, &pathArray);
		if (dirName == NULL) {
			logPrintf(LOG_ERROR, "doAction: path_split() failed\n");
			return 14;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL || containingDir->pendingCreate) {
			logPrintf(LOG_ERROR, "doAction: path not found when removing directory %s\n", action->path);
			return 15;
		}

		FilesystemDir* dir = findDir(containingDir, dirName);
		if (dir == NULL) {
			logPrintf(LOG_ERROR, "doAction: dir not found: %s\n", action->path);
			return 16;
		}

		action->prevAtime = dir->atime;
		action->prevMtime = dir->mtime;
		action->prevPendingCreate = dir->pendingCreate;
		if (dir->files.len > 0 || dir->dirs.len > 0) {
			if (!hasEntriesInRepository(dir) && !dir->pendingCreate) {
				dir->pendingCreate = 1;
				action->outcome = OutcomeLocalData;
			}
			logPrintf(LOG_WARNING, "doAction: directory %s removed by another client is not empty, keeping it\n", action->path);
			return 0;
		}

		if (removeFromDynArrayUnordered(&containingDir->dirs, (void*)dir) != 0) {
			logPrintf(LOG_ERROR, "doAction: removeFromDynArrayUnordered() failed\n");
			return 17;
		}
		freeDynArray(&dir->dirs);
		freeDynArray(&dir->files);
		free(dir);
		action->outcome = OutcomeApplied;
		return 0;

	} else if (action->actionType == ActionTypeSetTimes) {
		int64_t* atime;
		int64_t* mtime;
		if (findTimes(action->path, &atime, &mtime) != 0) {
			logPrintf(LOG_ERROR, "doAction: path not found when setting times of %s\n", action->path);
			return 24;
		}
		action->prevAtime = *atime;
		action->prevMtime = *mtime;
		if (action->hasAtime) {
			*atime = action->atime;
		}
		if (action->hasMtime) {
			*mtime = action->mtime;
		}
		action->outcome = OutcomeApplied;
		return 0;

	} else {
		logPrintf(LOG_ERROR, "doAction: unknown action type: %d\n", action->actionType);
		return -1;
	}

	return -1;
}

// A file created locally (not flushed yet) that undoAction() took out of the
// tree, because the entry that an undone action replaced is put back at its
// path. restoreStashedFiles() puts it back once the actions have been applied
// again.
typedef struct {
	char* path;
	FilesystemFile* file;
} StashedFile;

static DynArray stashedFiles;

static int stashFile(FilesystemFile* file)
{
	StashedFile* stashed = malloc(sizeof(StashedFile));
	if (stashed == NULL) {
		logPrintf(LOG_ERROR, "stashFile: malloc(): %s\n", strerror(errno));
		return 1;
	}
	stashed->path = getFullFilePath(file);
	if (stashed->path == NULL) {
		free(stashed);
		return 2;
	}
	stashed->file = file;
	if (addToDynArray(&stashedFiles, stashed) != 0) {
		free(stashed->path);
		free(stashed);
		return 3;
	}
	if (removeFromDynArrayUnordered(&file->parentDir->files, file) != 0) {
		logPrintf(LOG_ERROR, "stashFile: removeFromDynArrayUnordered() failed\n");
		// it stays in the tree, nothing to put back
		stashedFiles.len--;
		free(stashed->path);
		free(stashed);
		return 4;
	}
	// the directory may be removed before the file is put back
	file->parentDir = NULL;
	return 0;
}

static void freeFile(FilesystemFile* file)
{
	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		free(pw->buf);
		free(pw);
	}
	freeDynArray(&file->pendingWrites);
	if (file->dirtyFlags & DirtyFlagPendingCreate) {
		free((char*)file->name);
	}
	free(file);
}

// Puts a stashed file back at its path, following the rules for a local
// change that meets another client's action (see "Concurrent changes" in
// docs/design.md): a missing directory on the way is created as one that
// holds only local data, a directory at the path makes the file be renamed,
// and a file at the path is taken unless the stashed one has local data.
static void restoreStashedFile(StashedFile* stashed)
{
	FilesystemFile* file = stashed->file;
	DynArray pathArray;
	memset(&pathArray, 0, sizeof(DynArray));
	if (path_split(stashed->path, &pathArray) == NULL) {
		logPrintf(LOG_ERROR, "restoreStashedFile: path_split() failed, %s is lost\n", stashed->path);
		freeFile(file);
		return;
	}
	FilesystemDir* dir = root;
	for (int i=0; i<pathArray.len-1; i++) {
		const char* dirName = pathArray.objects[i];
		FilesystemDir* next = findDir(dir, dirName);
		if (next == NULL) {
			char* name = findFile(dir, dirName) != NULL
				? conflictName(dir, dirName) : strdup(dirName);
			next = malloc(sizeof(FilesystemDir));
			if (name == NULL || next == NULL || addToDynArray(&localDirNames, name) != 0) {
				logPrintf(LOG_ERROR, "restoreStashedFile: out of memory, %s is lost\n", stashed->path);
				free(name);
				free(next);
				path_free(&pathArray);
				freeFile(file);
				return;
			}
			memset(next, 0, sizeof(FilesystemDir));
			next->name = name;
			next->atime = next->mtime = file->mtime;
			next->parentDir = dir;
			next->pendingCreate = 1;
			addToDynArray(&dir->dirs, next);
			logPrintf(LOG_WARNING, "restoreStashedFile: directory %s was removed, keeping it for %s\n", name, stashed->path);
		}
		dir = next;
	}
	path_free(&pathArray);

	FilesystemFile* existing = findFile(dir, file->name);
	if (existing != NULL && !(existing->dirtyFlags & DirtyFlagPendingCreate)) {
		if (file->dirtyFlags == DirtyFlagPendingCreate) {
			// nothing written locally, take the file in the repository
			freeFile(file);
			return;
		}
		// keep the local one, its flush writes an editFile
		logPrintf(LOG_WARNING, "restoreStashedFile: file %s added by another client, keeping the local unflushed one\n", stashed->path);
		existing->atime = file->atime;
		existing->mtime = file->mtime;
		existing->content = file->content;
		existing->contentLen = file->contentLen;
		existing->size = file->size;
		existing->blockSize = file->blockSize;
		existing->dirtyFlags = file->dirtyFlags & ~DirtyFlagPendingCreate;
		existing->pendingWrites = file->pendingWrites;
		existing->pendingWritesSize = file->pendingWritesSize;
		existing->pendingWriteMtime = file->pendingWriteMtime;
		existing->truncSize = file->truncSize;
		free((char*)file->name);
		free(file);
		return;
	}
	if (existing != NULL || findDir(dir, file->name) != NULL) {
		char* newName = conflictName(dir, file->name);
		if (newName == NULL) {
			logPrintf(LOG_ERROR, "restoreStashedFile: conflictName() failed, %s is lost\n", stashed->path);
			freeFile(file);
			return;
		}
		logPrintf(LOG_WARNING, "restoreStashedFile: %s exists, renamed the local unflushed file to %s\n", stashed->path, newName);
		free((char*)file->name);
		file->name = newName;
	}
	file->parentDir = dir;
	addToDynArray(&dir->files, file);
}

static void restoreStashedFiles()
{
	for (int i=0; i<stashedFiles.len; i++) {
		StashedFile* stashed = stashedFiles.objects[i];
		restoreStashedFile(stashed);
		free(stashed->path);
		free(stashed);
	}
	freeDynArray(&stashedFiles);
}

// Reverts what doAction() (or the operation, for a local action) did, see
// Action.outcome. Actions are undone from the last one, so the tree is as
// the action left it, except for local changes that haven't been written yet:
// they are kept, by the same rules as for a remote action.
static int undoAction(Action* action)
{
	ActionOutcome outcome = action->outcome;
	logPrintf(LOG_VERBOSE_DEBUG, "undo action %d %s, outcome %d\n", action->actionType, action->path, outcome);

	action->outcome = OutcomeNotApplied;
	if (outcome == OutcomeNotApplied) {
		return 0;
	}

	if (action->actionType == ActionTypeSetTimes) {
		int64_t* atime;
		int64_t* mtime;
		if (findTimes(action->path, &atime, &mtime) != 0) {
			logPrintf(LOG_ERROR, "undoAction: path not found: %s\n", action->path);
			return 18;
		}
		*atime = action->prevAtime;
		*mtime = action->prevMtime;
		return 0;
	}

	const char* name;
	FilesystemDir* containingDir = findParent(action->path, &name);
	if (containingDir == NULL) {
		logPrintf(LOG_ERROR, "undoAction: path not found: %s\n", action->path);
		return 1;
	}
	FilesystemFile* file = findFile(containingDir, name);
	FilesystemDir* dir = findDir(containingDir, name);

	if (action->actionType == ActionTypeAddFile) {
		if (file == NULL) {
			logPrintf(LOG_ERROR, "undoAction: file not found: %s\n", action->path);
			return 2;
		}
		if (file->dirtyFlags == DirtyFlagNotDirty) {
			if (outcome == OutcomeApplied) {
				if (removeFromDynArrayUnordered(&containingDir->files, file) != 0) {
					logPrintf(LOG_ERROR, "undoAction: removeFromDynArrayUnordered() failed\n");
					return 3;
				}
				free(file);
				return 0;
			}
			// nothing written since: the file created locally as it was
			file->atime = action->prevAtime;
			file->mtime = action->prevMtime;
			file->content = action->prevContent;
			file->contentLen = action->prevContentLen;
			file->size = action->prevSize;
			file->blockSize = action->prevBlockSize;
		}
		// local changes on top of the added content: kept as a file that
		// isn't in the repository, applying the action again takes them
		return markFilePendingCreate(file) == 0 ? 0 : 4;

	} else if (action->actionType == ActionTypeEditFile) {
		if (file == NULL) {
			logPrintf(LOG_ERROR, "undoAction: file not found: %s\n", action->path);
			return 5;
		}
		// unflushed local changes stay on top of the old content
		file->mtime = action->prevMtime;
		if (action->hasAtime) {
			file->atime = action->prevAtime;
		}
		file->content = action->prevContent;
		file->contentLen = action->prevContentLen;
		file->size = action->prevSize;
		file->blockSize = action->prevBlockSize;
		return 0;

	} else if (action->actionType == ActionTypeRemoveFile) {
		if (outcome == OutcomeLocalData && file != NULL) {
			// back in the repository; its content is still the base of
			// its local changes
			if (file->dirtyFlags & DirtyFlagPendingCreate) {
				free((char*)file->name);
				file->name = name;
				file->dirtyFlags &= ~DirtyFlagPendingCreate;
			}
			return 0;
		}
		// the kept file was flushed and that action undone, or the file
		// was removed: add it again
		if (file != NULL) {
			if (!(file->dirtyFlags & DirtyFlagPendingCreate)) {
				logPrintf(LOG_ERROR, "undoAction: file already exists: %s\n", action->path);
				return 6;
			}
			// created locally after the removal
			if (stashFile(file) != 0) {
				return 7;
			}
		}
		if (dir != NULL) {
			if (!dir->pendingCreate) {
				logPrintf(LOG_ERROR, "undoAction: a directory with that name exists: %s\n", action->path);
				return 8;
			}
			if (renameLocalDir(containingDir, dir, name) != 0) {
				return 9;
			}
		}
		FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
		if (newFile == NULL) {
			logPrintf(LOG_ERROR, "undoAction: malloc(): %s\n", strerror(errno));
			return 10;
		}
		memset(newFile, 0, sizeof(FilesystemFile));
		newFile->name = name;
		newFile->atime = action->prevAtime;
		newFile->mtime = action->prevMtime;
		newFile->content = action->prevContent;
		newFile->contentLen = action->prevContentLen;
		newFile->size = action->prevSize;
		newFile->blockSize = action->prevBlockSize;
		newFile->dirtyFlags = DirtyFlagNotDirty;
		newFile->parentDir = containingDir;
		addToDynArray(&containingDir->files, newFile);
		return 0;

	} else if (action->actionType == ActionTypeAddDirectory) {
		if (dir == NULL) {
			logPrintf(LOG_ERROR, "undoAction: dir not found: %s\n", action->path);
			return 11;
		}
		if (outcome == OutcomeApplied && dir->files.len == 0 && dir->dirs.len == 0) {
			if (removeFromDynArrayUnordered(&containingDir->dirs, dir) != 0) {
				logPrintf(LOG_ERROR, "undoAction: removeFromDynArrayUnordered() failed\n");
				return 12;
			}
			freeDynArray(&dir->dirs);
			freeDynArray(&dir->files);
			free(dir);
			return 0;
		}
		// what is left in it is local data (what the actions after it
		// added has been undone): keep it for that
		if (hasEntriesInRepository(dir)) {
			logPrintf(LOG_ERROR, "undoAction: directory %s still has entries in the repository\n", action->path);
			return 13;
		}
		dir->pendingCreate = 1;
		return 0;

	} else if (action->actionType == ActionTypeRemoveDirectory) {
		if (outcome == OutcomeLocalData && dir != NULL) {
			dir->pendingCreate = 0;
			return 0;
		}
		// removed, or kept and then removed locally (rmdir of a directory
		// that holds only local data writes no action): add it again
		if (dir != NULL) {
			logPrintf(LOG_ERROR, "undoAction: directory already exists: %s\n", action->path);
			return 14;
		}
		if (file != NULL) {
			if (!(file->dirtyFlags & DirtyFlagPendingCreate)) {
				logPrintf(LOG_ERROR, "undoAction: a file with that name exists: %s\n", action->path);
				return 15;
			}
			// created locally after the removal
			if (stashFile(file) != 0) {
				return 16;
			}
		}
		FilesystemDir* newDir = malloc(sizeof(FilesystemDir));
		if (newDir == NULL) {
			logPrintf(LOG_ERROR, "undoAction: malloc(): %s\n", strerror(errno));
			return 17;
		}
		memset(newDir, 0, sizeof(FilesystemDir));
		newDir->name = name;
		newDir->atime = action->prevAtime;
		newDir->mtime = action->prevMtime;
		newDir->parentDir = containingDir;
		newDir->pendingCreate = outcome == OutcomeApplied ? action->prevPendingCreate : 0;
		addToDynArray(&containingDir->dirs, newDir);
		return 0;
	}

	logPrintf(LOG_ERROR, "undoAction: unknown action type: %d\n", action->actionType);
	return -1;
}

static DynArray actions;
static DynArray actionsPending;

// parses actions json document and appends actionsPending array with the results;
// returns non-zero if the document as a whole is invalid
static int parseAction(char* buf, size_t size)
{
	json_tokener* tokener = json_tokener_new();
	json_object* obj = json_tokener_parse_ex(tokener, buf, size);
	if (obj == NULL)
	{
		logPrintf(LOG_ERROR, "parseAction: json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		json_tokener_free(tokener);
		return 1;
	}
	json_tokener_free(tokener);

	if (json_object_get_type(obj) != json_type_array) {
		logPrintf(LOG_ERROR, "parseAction: document is not an array\n");
		json_object_put(obj);
		return 2;
	}

	// This json document is an array of action objects. Parse each of them and add to actionsPending.
	for (size_t i=0; i<json_object_array_length(obj); i++) {
		json_object* actionObj = json_object_array_get_idx(obj, i);

		if (json_object_get_type(actionObj) != json_type_object) {
			logPrintf(LOG_ERROR, "parseAction: array element is not an object\n");
			continue;
		}

		// parse time
		json_object* timeField;
		if (json_object_object_get_ex(actionObj, "time", &timeField) == 0) {
			logPrintf(LOG_ERROR, "parseAction: action object doesn't have 'time' field\n");
			continue;
		}
		if (json_object_get_type(timeField) != json_type_int) {
			logPrintf(LOG_ERROR, "parseAction: 'time' field is not an integer\n");
			continue;
		}
		int64_t time = json_object_get_int64(timeField);

		// parse size
		json_object* sizeField;
		int64_t size = 0;
		if (json_object_object_get_ex(actionObj, "size", &sizeField) != 0) {
			if (json_object_get_type(sizeField) != json_type_int) {
				logPrintf(LOG_ERROR, "parseAction: 'size' field is not an integer\n");
				continue;
			}
			size = json_object_get_int64(sizeField);
		}

		// parse blockSize
		json_object* blockSizeField;
		int64_t blockSize = 0;
		if (json_object_object_get_ex(actionObj, "blockSize", &blockSizeField) != 0) {
			if (json_object_get_type(blockSizeField) != json_type_int) {
				logPrintf(LOG_ERROR, "parseAction: 'blockSize' field is not an integer\n");
				continue;
			}
			blockSize = json_object_get_int64(blockSizeField);
		}

		// parse mtime and atime (optional)
		json_object* mtimeField;
		int hasMtime = json_object_object_get_ex(actionObj, "mtime", &mtimeField) != 0;
		if (hasMtime && json_object_get_type(mtimeField) != json_type_int) {
			logPrintf(LOG_ERROR, "parseAction: 'mtime' field is not an integer\n");
			continue;
		}
		json_object* atimeField;
		int hasAtime = json_object_object_get_ex(actionObj, "atime", &atimeField) != 0;
		if (hasAtime && json_object_get_type(atimeField) != json_type_int) {
			logPrintf(LOG_ERROR, "parseAction: 'atime' field is not an integer\n");
			continue;
		}

		// parse action
		json_object* actionTypeField;
		if (json_object_object_get_ex(actionObj, "action", &actionTypeField) == 0) {
			logPrintf(LOG_ERROR, "parseAction: action object doesn't have 'action' field\n");
			continue;
		}
		if (json_object_get_type(actionTypeField) != json_type_string) {
			logPrintf(LOG_ERROR, "parseAction: 'action' field is not a string\n");
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
		} else if (strcmp(actionTypeStr, "editFile") == 0) {
			actionType = ActionTypeEditFile;
		} else if (strcmp(actionTypeStr, "setTimes") == 0) {
			actionType = ActionTypeSetTimes;
		} else {
			logPrintf(LOG_ERROR, "parseAction: unknown action\n");
			continue;
		}

		// parse path
		json_object* pathField;
		if (json_object_object_get_ex(actionObj, "path", &pathField) == 0) {
			logPrintf(LOG_ERROR, "parseAction: action object doesn't have 'path' field\n");
			continue;
		}
		if (json_object_get_type(pathField) != json_type_string) {
			logPrintf(LOG_ERROR, "parseAction: 'path' field is not a string\n");
			continue;
		}
		const char* path = json_object_get_string(pathField);

		// parse content
		json_object* contentField;
		if (json_object_object_get_ex(actionObj, "content", &contentField) == 0) {
			logPrintf(LOG_ERROR, "parseAction: action object doesn't have 'content' field\n");
			continue;
		}
		if (json_object_get_type(contentField) != json_type_array) {
			logPrintf(LOG_ERROR, "parseAction: 'content' field is not an array\n");
			continue;
		}
		size_t contentLen = json_object_array_length(contentField);
		char* content = malloc(contentLen * MAX_STORAGE_NAME_LEN);
		if (content == NULL) {
			logPrintf(LOG_ERROR, "parseAction: malloc(): %s\n", strerror(errno));
			continue;
		}
		size_t j;
		for (j=0; j<contentLen; j++) {
			json_object* contentItemField = json_object_array_get_idx(contentField, j);

			if (json_object_get_type(contentItemField) != json_type_string) {
				logPrintf(LOG_ERROR, "parseAction: 'content' contents is not a string\n");
				break;
			}
			const char* contentItemStr = json_object_get_string(contentItemField);
			snprintf(content + (MAX_STORAGE_NAME_LEN * j), MAX_STORAGE_NAME_LEN, "%s", contentItemStr);
		}
		if (j != contentLen) {
			free(content);
			continue;
		}

		// size, blockSize and content come from the (untrusted) server:
		// every block of the file's data must be in content, and a block
		// can't be larger than bucse-mount ever makes it. Every version
		// writes a power of 2 from MIN_BLOCK_SIZE to MAX_BLOCK_SIZE.
		if (actionType == ActionTypeAddFile || actionType == ActionTypeEditFile) {
			int validBlockSize = blockSize == 0 || (blockSize >= MIN_BLOCK_SIZE
				&& blockSize <= MAX_BLOCK_SIZE && (blockSize & (blockSize - 1)) == 0);
			if (size < 0 || !validBlockSize || (size > 0 && blockSize == 0)) {
				logPrintf(LOG_ERROR, "parseAction: invalid size %" PRId64 " or blockSize %" PRId64 "\n",
					size, blockSize);
				free(content);
				continue;
			}
			if (size > 0 && (uint64_t)((size - 1) / blockSize) >= contentLen) {
				logPrintf(LOG_ERROR, "parseAction: content has %zu blocks, too few for size %" PRId64 " and blockSize %" PRId64 "\n",
					contentLen, size, blockSize);
				free(content);
				continue;
			}
		}

		// create new action object
		Action* newAction = calloc(1, sizeof(Action));
		if (newAction == NULL) {
			logPrintf(LOG_ERROR, "parseAction: malloc(): %s\n", strerror(errno));
			free(content);
			continue;
		}
		newAction->time = time;
		newAction->actionType = actionType;
		newAction->path = malloc(strlen(path) + 1);
		if (newAction->path == NULL) {
			logPrintf(LOG_ERROR, "parseAction: malloc(): %s\n", strerror(errno));
			free(content);
			free(newAction);
			continue;
		}
		memcpy(newAction->path, path, strlen(path) + 1);
		newAction->content = content;
		newAction->contentLen = contentLen;
		newAction->size = size;
		newAction->blockSize = blockSize;
		newAction->hasMtime = hasMtime;
		newAction->mtime = hasMtime ? json_object_get_int64(mtimeField) : 0;
		newAction->hasAtime = hasAtime;
		newAction->atime = hasAtime ? json_object_get_int64(atimeField) : 0;
		newAction->outcome = OutcomeNotApplied;

		addToDynArray(&actionsPending, newAction);
	}

	json_object_put(obj);
	return 0;
}

int actionAdded(char* actionName, char* buf, size_t size)
{
	//logPrintf(LOG_DEBUG, "actionAdded(): %s\n  %s\n  %d\n", actionName, buf, size);

	// TODO: parseAction() not needed, move it here
	return parseAction(buf, size);
}

void flushActionsAdded()
{
	// early out if there is no pending action
	if (actionsPending.len == 0) {
		return;
	}

	// prepare actionsPending
	qsort(actionsPending.objects, actionsPending.len, sizeof(void*), compareActionPtrs);
	printActions(&actionsPending);

	// Check for duplicates: actions in actionsPending that already are in actions.
	// This can occur only if there are out of order actions, hence this if statement
	int i = actionsPending.len - 1;
	int j = actions.len - 1;
	int dups = 0;
	if (actions.len > 0 && actionsPending.len > 0
		&& ((Action*)actions.objects[actions.len - 1])->time >= // time of last action in actions
			((Action*)actionsPending.objects[0])->time // time of first action in actionsPending
	      ) {
		logPrintf(LOG_VERBOSE_DEBUG, "flushActionsAdded: duplicates\n");
		while (i >=0 && j >= 0) {
			// pick next "largest" item
			if (i==0 || compareActionPtrs(&actions.objects[j], &actionsPending.objects[i-1]) >= 0)
			{
				// compare with actions
				int c = compareActionPtrs(&actions.objects[j], &actionsPending.objects[i]);
				if (c > 0) {
					j--;
				} else if (c < 0) {
					i--;
				} else {
					logPrintf(LOG_VERBOSE_DEBUG, "flushActionsAdded: dup found\n");
					freeAction(actionsPending.objects[i]);
					removeFromDynArrayUnorderedByIndex(&actionsPending, i);
					dups++;
					i--;
				}
			} else {
				// compare with previous actionsPending
				int c = compareActionPtrs(&actionsPending.objects[i], &actionsPending.objects[i-1]);
				if (c == 0) {
					logPrintf(LOG_VERBOSE_DEBUG, "flushActionsAdded: dup found\n");
					freeAction(actionsPending.objects[i]);
					removeFromDynArrayUnorderedByIndex(&actionsPending, i);
					dups++;
				}
				i--;
			}
		}

	}
	while (i>0) {
		int c = compareActionPtrs(&actionsPending.objects[i], &actionsPending.objects[i-1]);
		if (c == 0) {
			logPrintf(LOG_VERBOSE_DEBUG, "flushActionsAdded: dup found\n");
			freeAction(actionsPending.objects[i]);
			removeFromDynArrayUnorderedByIndex(&actionsPending, i);
			dups++;
		}
		i--;
	}
	if (dups > 0) {
		logPrintf(LOG_VERBOSE_DEBUG, "flushActionsAdded: duplicates: removed %d pending actions\n", dups);
		qsort(actionsPending.objects, actionsPending.len, sizeof(void*), compareActionPtrs);
	}

	// Handle out of order actions: undo the actions that are ordered after
	// the first new one, from the last one, and apply them again together
	// with the new ones. Everything happens with bucseMutex held, so no
	// operation sees the tree in between.
	int outOfOrderActions = 0;
	while (actions.len > 0 && actionsPending.len > 0
		&& ((Action*)actions.objects[actions.len - 1])->time >= // time of last action in actions
			((Action*)actionsPending.objects[0])->time // time of first action in actionsPending
	      ) {
		logPrintf(LOG_DEBUG, "undoing action due to out of order action!\n");
		// move last action to actionsPending
		addToDynArray(&actionsPending, actions.objects[actions.len - 1]);
		actions.len--;

		if (undoAction(actions.objects[actions.len]) != 0) {
			logPrintf(LOG_ERROR, "flushActionsAdded: undoAction() failed\n");
		}

		outOfOrderActions++;
	}

	if (outOfOrderActions > 0) {
		qsort(actionsPending.objects, actionsPending.len, sizeof(void*), compareActionPtrs);
	}

	// move actions from actinsPending to actions, acting on them
	for (int i=0; i<actionsPending.len; i++) {
		addToDynArray(&actions, actionsPending.objects[i]);
		doAction(actionsPending.objects[i]);
	}
	freeDynArray(&actionsPending);

	// local files that undoAction() took out of the tree
	restoreStashedFiles();
}

void actionsCleanup()
{
	for (int i=0; i<actions.len; i++) {
		freeAction(actions.objects[i]);
	}
	freeDynArray(&actions);

	// free actionsPending
	for (int i=0; i<actionsPending.len; i++) {
		freeAction(actionsPending.objects[i]);
	}
	freeDynArray(&actionsPending);

	for (int i=0; i<localDirNames.len; i++) {
		free(localDirNames.objects[i]);
	}
	freeDynArray(&localDirNames);
}

char* serializeAction(Action* action)
{
	json_object* jsonNewActions = json_object_new_array();
	if (!jsonNewActions) {
		logPrintf(LOG_ERROR, "serializeAction: can't create JSON array\n");
		return NULL;
	}
	// from here on, every value is added to jsonNewActions (or to an object
	// in it) right after it is created, so releasing jsonNewActions releases
	// everything; a value that can't be added is released on its own
	// (json_object_put(NULL) is a no-op)
	json_object* jsonNewAction = json_object_new_object();
	if (jsonNewAction == NULL || json_object_array_add(jsonNewActions, jsonNewAction) != 0) {
		json_object_put(jsonNewAction);
		goto error;
	}

	json_object* val = json_object_new_int64(action->time);
	if (val == NULL || json_object_object_add(jsonNewAction, "time", val) != 0) {
		json_object_put(val);
		goto error;
	}

	const char* actionStr = NULL;
	if (action->actionType == ActionTypeAddFile) {
		actionStr = "addFile";
	} else if (action->actionType == ActionTypeRemoveFile) {
		actionStr = "removeFile";
	} else if (action->actionType == ActionTypeAddDirectory) {
		actionStr = "addDirectory";
	} else if (action->actionType == ActionTypeRemoveDirectory) {
		actionStr = "removeDirectory";
	} else if (action->actionType == ActionTypeEditFile) {
		actionStr = "editFile";
	} else if (action->actionType == ActionTypeSetTimes) {
		actionStr = "setTimes";
	} else {
		actionStr = "unknown";
	}
	val = json_object_new_string(actionStr);
	if (val == NULL || json_object_object_add(jsonNewAction, "action", val) != 0) {
		json_object_put(val);
		goto error;
	}

	val = json_object_new_string(action->path);
	if (val == NULL || json_object_object_add(jsonNewAction, "path", val) != 0) {
		json_object_put(val);
		goto error;
	}

	json_object* jsonNewContent = json_object_new_array();
	if (jsonNewContent == NULL || json_object_object_add(jsonNewAction, "content", jsonNewContent) != 0) {
		json_object_put(jsonNewContent);
		goto error;
	}
	for (int i=0; i<action->contentLen; i++) {
		val = json_object_new_string(action->content + i*MAX_STORAGE_NAME_LEN);
		if (val == NULL || json_object_array_add(jsonNewContent, val) != 0) {
			json_object_put(val);
			goto error;
		}
	}

	val = json_object_new_int64(action->size);
	if (val == NULL || json_object_object_add(jsonNewAction, "size", val) != 0) {
		json_object_put(val);
		goto error;
	}

	val = json_object_new_int(action->blockSize);
	if (val == NULL || json_object_object_add(jsonNewAction, "blockSize", val) != 0) {
		json_object_put(val);
		goto error;
	}

	if (action->hasMtime) {
		val = json_object_new_int64(action->mtime);
		if (val == NULL || json_object_object_add(jsonNewAction, "mtime", val) != 0) {
			json_object_put(val);
			goto error;
		}
	}

	if (action->hasAtime) {
		val = json_object_new_int64(action->atime);
		if (val == NULL || json_object_object_add(jsonNewAction, "atime", val) != 0) {
			json_object_put(val);
			goto error;
		}
	}

	const char* jsonData = json_object_to_json_string_ext(
		jsonNewActions, JSON_C_TO_STRING_PRETTY);
	if (jsonData == NULL) {
		goto error;
	}

	char* result = malloc(strlen(jsonData)+1);
	if (result == NULL) {
		logPrintf(LOG_ERROR, "serializeAction: malloc(): %s\n", strerror(errno));
		json_object_put(jsonNewActions);
		return NULL;
	}
	memcpy(result, jsonData, strlen(jsonData)+1);

	json_object_put(jsonNewActions);

	return result;

error:
	logPrintf(LOG_ERROR, "serializeAction: can't build JSON\n");
	json_object_put(jsonNewActions);
	return NULL;
}

// Returns the time for a new local action: the current time, but always
// later than every action already applied (a Lamport clock). So a new action
// is ordered after everything this client has seen, even if gettimeofday()
// stepped back or another client's clock is ahead; otherwise every client
// would apply it before them (e.g. a delete before the edit it follows).
int64_t getNewActionTime()
{
	int64_t t = getCurrentTime();
	if (actions.len > 0) {
		int64_t last = ((Action*)actions.objects[actions.len - 1])->time;
		if (t <= last) {
			t = last + 1;
		}
	}
	return t;
}

// Sets what undoAction() needs to revert a local action. It is called before
// the operation changes the tree, so the entry at the action's path is still
// the one that the action replaces.
static void setLocalOutcome(Action* action)
{
	action->outcome = OutcomeNotApplied;
	const char* name;
	FilesystemDir* containingDir = findParent(action->path, &name);
	FilesystemFile* file = containingDir ? findFile(containingDir, name) : NULL;
	FilesystemDir* dir = containingDir ? findDir(containingDir, name) : NULL;
	int64_t* atime;
	int64_t* mtime;

	if (action->actionType == ActionTypeAddFile) {
		// a file created locally is flushed, or a file renamed: what
		// was local is in the action now
		action->outcome = OutcomeApplied;
	} else if (action->actionType == ActionTypeEditFile && file != NULL) {
		savePrevFile(action, file);
		action->outcome = OutcomeApplied;
	} else if (action->actionType == ActionTypeRemoveFile && file != NULL) {
		savePrevFile(action, file);
		action->outcome = OutcomeApplied;
	} else if (action->actionType == ActionTypeAddDirectory) {
		// recreatePendingDirs() adds a directory that holds local data
		action->outcome = (dir != NULL && dir->pendingCreate)
			? OutcomeLocalData : OutcomeApplied;
	} else if (action->actionType == ActionTypeRemoveDirectory && dir != NULL) {
		action->prevAtime = dir->atime;
		action->prevMtime = dir->mtime;
		action->prevPendingCreate = dir->pendingCreate;
		action->outcome = OutcomeApplied;
	} else if (action->actionType == ActionTypeSetTimes
		&& findTimes(action->path, &atime, &mtime) == 0) {
		action->prevAtime = *atime;
		action->prevMtime = *mtime;
		action->outcome = OutcomeApplied;
	} else {
		logPrintf(LOG_ERROR, "setLocalOutcome: %s not found, the action can't be undone\n", action->path);
	}
}

// Adds a local action. An action with the same time as the last ones (the
// second action of a rename) is put among them in the order of
// compareActions(), so that actions stays sorted the way flushActionsAdded()
// expects.
void addAction(Action *newAction)
{
	setLocalOutcome(newAction);
	if (addToDynArray(&actions, newAction) != 0) {
		return;
	}
	int i = actions.len - 1;
	while (i > 0) {
		Action* prev = actions.objects[i-1];
		if (prev->time != newAction->time || compareActions(prev, newAction) <= 0) {
			break;
		}
		actions.objects[i] = prev;
		actions.objects[i-1] = newAction;
		i--;
	}
}

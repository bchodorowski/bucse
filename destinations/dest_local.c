/*
 * destinations/dest_local.c
 *
 * An implementation of the local destination -- repository files are stored as
 * files in the filesystem.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

#include <json.h>

#include "../log.h"

#include "dest.h"

static char* repositoryPath;
static char* repositoryJsonFilePath;
static char* repositoryFilePath;
static char* repositoryActionsPath;
static char* repositoryStoragePath;

static ActionAddedCallback cachedActionAddedCallback;

// TODO: the same code is in dest_ssh.c. Fix it.
// TODO: rename to ActionNames [?]
typedef struct {
	char* names;
	int len;
	int size;
} Actions;

static int addAction(Actions *actions, char* newActionName)
{
	int oldActionsSize = actions->size;

	if (actions->len == actions->size) {
		int newActionSize;

		if (actions->size == 0) {
			newActionSize = 16;
		} else {
			newActionSize = oldActionsSize * 2;
		}

		char* newNames = malloc(newActionSize * MAX_ACTION_NAME_LEN);
		if (newNames == NULL) {
			logPrintf(LOG_ERROR, "addAction: malloc(): %s\n", strerror(errno));
			return 1;
		}
		if (actions->names != NULL) {
			memcpy(newNames, actions->names, oldActionsSize * MAX_ACTION_NAME_LEN);
			free(actions->names);
		}
		actions->names = newNames;
		actions->size = newActionSize;
	}

	snprintf(actions->names + (MAX_ACTION_NAME_LEN * actions->len), MAX_ACTION_NAME_LEN, "%s", newActionName);
	actions->len++;

	return 0;
}

static void freeActions(Actions *actions)
{
	if (actions->names != NULL) {
		free(actions->names);
	}
	actions->names = NULL;
	actions->len = actions->size = 0;
}

static char* getAction(Actions *actions, int index)
{
	return actions->names + (MAX_ACTION_NAME_LEN * index);
}

static int findAction(Actions *actions, char* actionNameToFound)
{
	for (int i=0; i<actions->len; i++) {
		if (strncmp(actions->names + (MAX_ACTION_NAME_LEN * i), actionNameToFound, MAX_ACTION_NAME_LEN) == 0) {
			return i;
		}
	}
	return -1;
}

static Actions handledActions;

int destLocalInit(char* repository)
{
	// construct file path of repository.json file
	repositoryJsonFilePath = malloc(MAX_FILEPATH_LEN);
	if (repositoryJsonFilePath == NULL) {
		logPrintf(LOG_ERROR, "destLocalInit: malloc(): %s\n", strerror(errno));

		return 1;
	}
	repositoryFilePath = malloc(MAX_FILEPATH_LEN);
	if (repositoryFilePath == NULL) {
		logPrintf(LOG_ERROR, "destLocalInit: malloc(): %s\n", strerror(errno));
		free(repositoryJsonFilePath);

		return 2;
	}
	repositoryActionsPath = malloc(MAX_FILEPATH_LEN);
	if (repositoryActionsPath == NULL) {
		logPrintf(LOG_ERROR, "destLocalInit: malloc(): %s\n", strerror(errno));

		free(repositoryJsonFilePath);
		free(repositoryFilePath);
		return 3;
	}
	repositoryStoragePath = malloc(MAX_FILEPATH_LEN);
	if (repositoryStoragePath == NULL) {
		logPrintf(LOG_ERROR, "destLocalInit: malloc(): %s\n", strerror(errno));

		free(repositoryJsonFilePath);
		free(repositoryFilePath);
		free(repositoryActionsPath);
		return 4;
	}

	repositoryPath = strdup(repository);
	if (repositoryPath == NULL) {
		free(repositoryJsonFilePath);
		free(repositoryFilePath);
		free(repositoryActionsPath);
		free(repositoryStoragePath);
		return 5;
	}

	snprintf(repositoryJsonFilePath, MAX_FILEPATH_LEN, "%s/repository.json", repository);
	snprintf(repositoryFilePath, MAX_FILEPATH_LEN, "%s/repository", repository);
	snprintf(repositoryActionsPath, MAX_FILEPATH_LEN, "%s/actions", repository);
	snprintf(repositoryStoragePath, MAX_FILEPATH_LEN, "%s/storage", repository);

	return 0;
}

void destLocalShutdown()
{
	if (repositoryJsonFilePath != NULL) {
		free(repositoryJsonFilePath);
		repositoryJsonFilePath = NULL;
	}
	if (repositoryFilePath != NULL) {
		free(repositoryFilePath);
		repositoryFilePath = NULL;
	}
	if (repositoryActionsPath != NULL) {
		free(repositoryActionsPath);
		repositoryActionsPath = NULL;
	}
	if (repositoryStoragePath != NULL) {
		free(repositoryStoragePath);
		repositoryStoragePath = NULL;
	}
	if (repositoryPath != NULL) {
		free(repositoryPath);
		repositoryPath = NULL;
	}

	freeActions(&handledActions);
}

int destLocalCreateDirs()
{
	struct stat s;
	int err;

	if (mkdir(repositoryPath, S_IRUSR | S_IWUSR | S_IXUSR
		| S_IRGRP | S_IXGRP
		| S_IROTH | S_IXOTH) != 0) {
		logPrintf(LOG_ERROR, "destLocalCreateDirs: mkdir(): %s\n", strerror(errno));
		return 1;
	}

	if (mkdir(repositoryActionsPath, S_IRUSR | S_IWUSR | S_IXUSR
		| S_IRGRP | S_IXGRP
		| S_IROTH | S_IXOTH) != 0) {
		logPrintf(LOG_ERROR, "destLocalCreateDirs: mkdir(): %s\n", strerror(errno));
		return 2;
	}

	if (mkdir(repositoryStoragePath, S_IRUSR | S_IWUSR | S_IXUSR
		| S_IRGRP | S_IXGRP
		| S_IROTH | S_IXOTH) != 0) {
		logPrintf(LOG_ERROR, "destLocalCreateDirs: mkdir(): %s\n", strerror(errno));
		return 3;
	}

	// check if repository json file already exists
	errno = 0;
	err = stat(repositoryJsonFilePath, &s);
	if (err != 0 && errno != ENOENT) {
		logPrintf(LOG_ERROR, "destLocalCreateDirs: stat(): %s\n", strerror(errno));
		return 4;
	} else if (err != 0 && errno == ENOENT) {
		// OK
	} else {
		logPrintf(LOG_ERROR, "destLocalCreateDirs: repository.json file already exists\n", strerror(errno));
		return 5;
	}

	// check if repository file already exists
	errno = 0;
	err = stat(repositoryFilePath, &s);
	if (err != 0 && errno != ENOENT) {
		logPrintf(LOG_ERROR, "destLocalCreateDirs: stat(): %s\n", strerror(errno));
		return 6;
	} else if (err != 0 && errno == ENOENT) {
		// OK
	} else {
		logPrintf(LOG_ERROR, "destLocalCreateDirs: repository file already exists\n", strerror(errno));
		return 7;
	}

	return 0;
}

int destLocalPutStorageFile(const char* filename, char *buf, size_t size)
{
	char* storageFilePath = malloc(MAX_FILEPATH_LEN);
	if (storageFilePath == NULL) {
		logPrintf(LOG_ERROR, "destLocalPutStorageFile: malloc(): %s\n", strerror(errno));

		return 1;
	}

	snprintf(storageFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryStoragePath, filename);

	FILE* file = fopen(storageFilePath, "wb");
	free(storageFilePath);
	if (file == NULL) {
		logPrintf(LOG_ERROR, "destLocalPutStorageFile: fopen(): %s\n", strerror(errno));
		return 2;
	}

	size_t bytesWritten = 0;
	while (!ferror(file) && bytesWritten < size) {
		bytesWritten += fwrite(buf + bytesWritten, 1, size - bytesWritten, file);
	}
	if (ferror(file)) {
		logPrintf(LOG_ERROR, "destLocalPutStorageFile: ferror() returned a non-zero value\n");
		fclose(file);
		return 3;
	}
	fclose(file);

	return 0;
}

int destLocalGetStorageFile(const char* filename, char *buf, size_t *size)
{
	char* storageFilePath = malloc(MAX_FILEPATH_LEN);
	if (storageFilePath == NULL) {
		logPrintf(LOG_ERROR, "destLocalGetStorageFile: malloc(): %s\n", strerror(errno));

		return 1;
	}

	snprintf(storageFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryStoragePath, filename);

	FILE* file = fopen(storageFilePath, "r");
	free(storageFilePath);

	if (file == NULL) {
		logPrintf(LOG_ERROR, "destLocalGetStorageFile: fopen(): %s\n", strerror(errno));
		return 2;
	}

	int bytesRead = 0;
	while (!feof(file) && !ferror(file) && bytesRead < *size) {
		bytesRead += fread(buf + bytesRead, 1, *size - bytesRead, file);
	}
	if (ferror(file)) {
		logPrintf(LOG_ERROR, "destLocalGetStorageFile: ferror() returned a non-zero value\n");
		fclose(file);
		return 3;
	}
	fclose(file);

	if (bytesRead >= *size) {
		logPrintf(LOG_ERROR, "destLocalGetStorageFile: repository.json file is too large\n");

		return 4;
	}

	buf[bytesRead] = 0; // null termination
	*size = bytesRead;
	return 0;
}

int destLocalAddActionFile(char* filename, char *buf, size_t size)
{
	char* actionFilePath = malloc(MAX_FILEPATH_LEN);
	if (actionFilePath == NULL) {
		logPrintf(LOG_ERROR, "destLocalAddActionFile: malloc(): %s\n", strerror(errno));

		return 1;
	}

	snprintf(actionFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryActionsPath, filename);

	FILE* file = fopen(actionFilePath, "wb");
	free(actionFilePath);
	if (file == NULL) {
		logPrintf(LOG_ERROR, "destLocalAddActionFile: fopen(): %s\n", strerror(errno));
		return 2;
	}

	size_t bytesWritten = 0;
	while (!ferror(file) && bytesWritten < size) {
		bytesWritten += fwrite(buf + bytesWritten, 1, size - bytesWritten, file);
	}
	if (ferror(file)) {
		logPrintf(LOG_ERROR, "destLocalAddActionFile: ferror() returned a non-zero value\n");
		fclose(file);
		return 3;
	}
	fclose(file);

	addAction(&handledActions, filename);
	return 0;
}

int destLocalPutRepositoryJsonFile(char *buf, size_t size)
{
	FILE* file = fopen(repositoryJsonFilePath, "wb");
	if (file == NULL) {
		logPrintf(LOG_ERROR, "destLocalPutRepositoryJsonFile: fopen(): %s\n", strerror(errno));
		return 1;
	}

	size_t bytesWritten = 0;
	while (!ferror(file) && bytesWritten < size) {
		bytesWritten += fwrite(buf + bytesWritten, 1, size - bytesWritten, file);
	}
	if (ferror(file)) {
		logPrintf(LOG_ERROR, "destLocalPutRepositoryJsonFile: ferror() returned a non-zero value\n");
		fclose(file);
		return 2;
	}
	fclose(file);

	return 0;
}

int destLocalGetRepositoryJsonFile(char *buf, size_t *size)
{
	FILE* file = fopen(repositoryJsonFilePath, "r");
	if (file == NULL) {
		logPrintf(LOG_ERROR, "destLocalGetRepositoryJsonFile: fopen(): %s\n", strerror(errno));

		return 1;
	}

	int bytesRead = 0;
	while (!feof(file) && !ferror(file) && bytesRead < *size) {
		bytesRead += fread(buf + bytesRead, 1, *size - bytesRead, file);
	}
	if (ferror(file)) {
		logPrintf(LOG_ERROR, "destLocalGetRepositoryJsonFile: ferror() returned a non-zero value\n");
		fclose(file);
		return 2;
	}
	fclose(file);

	if (bytesRead >= *size) {
		logPrintf(LOG_ERROR, "destLocalGetRepositoryJsonFile: repository.json file is too large\n");

		return 3;
	}

	buf[bytesRead] = 0; // null termination
	*size = bytesRead;
	return 0;
}

int destLocalPutRepositoryFile(char *buf, size_t size)
{
	FILE* file = fopen(repositoryFilePath, "wb");
	if (file == NULL) {
		logPrintf(LOG_ERROR, "destLocalPutRepositoryFile: fopen(): %s\n", strerror(errno));
		return 1;
	}

	size_t bytesWritten = 0;
	while (!ferror(file) && bytesWritten < size) {
		bytesWritten += fwrite(buf + bytesWritten, 1, size - bytesWritten, file);
	}
	if (ferror(file)) {
		logPrintf(LOG_ERROR, "destLocalPutRepositoryFile: ferror() returned a non-zero value\n");
		fclose(file);
		return 2;
	}
	fclose(file);

	return 0;
}

int destLocalGetRepositoryFile(char *buf, size_t *size)
{
	FILE* file = fopen(repositoryFilePath, "r");
	if (file == NULL) {
		logPrintf(LOG_ERROR, "destLocalGetRepositoryFile: fopen(): %s\n", strerror(errno));

		return 1;
	}

	int bytesRead = 0;
	while (!feof(file) && !ferror(file) && bytesRead < *size) {
		bytesRead += fread(buf + bytesRead, 1, *size - bytesRead, file);
	}
	if (ferror(file)) {
		logPrintf(LOG_ERROR, "destLocalGetRepositoryFile: ferror() returned a non-zero value\n");
		fclose(file);
		return 2;
	}
	fclose(file);

	if (bytesRead >= *size) {
		logPrintf(LOG_ERROR, "destLocalGetRepositoryFile: repository file is too large\n");

		return 3;
	}

	buf[bytesRead] = 0; // null termination
	*size = bytesRead;
	return 0;
}

int destLocalSetCallbackActionAdded(ActionAddedCallback callback)
{
	cachedActionAddedCallback = callback;
	return 0;
}

int destLocalIsTickable()
{
	return 1;
}

int destLocalTick()
{
#define TICK_PERIOD_SECONDS 10

	static int counter = 0;
	if (--counter > 0) {
		return 0;
	}
	counter = TICK_PERIOD_SECONDS;

	DIR *actionsDir = opendir(repositoryActionsPath);
	if (actionsDir == NULL) {
		logPrintf(LOG_ERROR, "warning: destLocalTick(): opendir(): %s\n", strerror(errno));
		return 0;
	}

	Actions newActions;
	memset(&newActions, 0, sizeof(Actions));

	for (;;) {
		errno = 0;
		struct dirent* actionDir = readdir(actionsDir);
		if (actionDir == NULL && errno == 0) {
			break;
		} else if (errno) {
			logPrintf(LOG_ERROR, "warning: destLocalTick(): readdir(): %s\n", strerror(errno));

			closedir(actionsDir);
			return 0;
		}
		if (actionDir->d_name && actionDir->d_name[0] == '.') {
			continue;
		}

		// TODO: consider optimizing by keeping handledActions sorted and searching with binary search
		
		// is the action not already handled?
		if (findAction(&handledActions, actionDir->d_name) == -1) {
			addAction(&newActions, actionDir->d_name);
		}
	}
	closedir(actionsDir);

	logPrintf(LOG_DEBUG, "new actions count: %d\n", newActions.len);

	char* actionFilePath = malloc(MAX_FILEPATH_LEN);
	if (actionFilePath == NULL) {
		logPrintf(LOG_ERROR, "destLocalTick: malloc(): %s\n", strerror(errno));

		return 1;
	}
	char* actionFileBuf = malloc(MAX_ACTION_LEN);
	if (actionFileBuf == NULL) {
		logPrintf(LOG_ERROR, "destLocalTick: malloc(): %s\n", strerror(errno));

		free(actionFilePath);
		return 1;
	}

	for (int i=0; i<newActions.len; i++) {
		logPrintf(LOG_VERBOSE_DEBUG, "handle new action: %s\n", getAction(&newActions, i));

		snprintf(actionFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryActionsPath, getAction(&newActions, i));

		FILE* file = fopen(actionFilePath, "r");
		if (file == NULL) {
			logPrintf(LOG_ERROR, "destLocalTick: fopen(): %s\n", strerror(errno));
			continue;
		}

		size_t bytesRead = 0;
		while (!feof(file) && !ferror(file) && bytesRead < MAX_ACTION_LEN) {
			bytesRead += fread(actionFileBuf + bytesRead, 1, MAX_ACTION_LEN - bytesRead, file);
		}
		if (ferror(file)) {
			logPrintf(LOG_ERROR, "destLocalTick: ferror() returned a non-zero value\n");
			fclose(file);
			continue;
		}
		fclose(file);

		if (bytesRead >= MAX_ACTION_LEN) {
			logPrintf(LOG_ERROR, "destLocalTick: action file is too large\n");
			continue;
		}

		actionFileBuf[bytesRead] = 0; // null termination
		if (cachedActionAddedCallback) {
			cachedActionAddedCallback(getAction(&newActions, i), actionFileBuf, bytesRead, newActions.len - i - 1);
		} else {
			logPrintf(LOG_ERROR, "destLocalTick: no action added callback\n");
		}
	}
	free(actionFilePath);
	free(actionFileBuf);
	
	for (int i=0; i<newActions.len; i++) {
		addAction(&handledActions, getAction(&newActions, i));
	}


	freeActions(&newActions);

	return 0;
}

Destination destinationLocal = {
	.init = destLocalInit,
	.shutdown = destLocalShutdown,
	.createDirs = destLocalCreateDirs,
	.putStorageFile = destLocalPutStorageFile,
	.getStorageFile = destLocalGetStorageFile,
	.addActionFile = destLocalAddActionFile,
	.putRepositoryJsonFile = destLocalPutRepositoryJsonFile,
	.getRepositoryJsonFile = destLocalGetRepositoryJsonFile,
	.putRepositoryFile = destLocalPutRepositoryFile,
	.getRepositoryFile = destLocalGetRepositoryFile,
	.setCallbackActionAdded = destLocalSetCallbackActionAdded,
	.isTickable = destLocalIsTickable,
	.tick = destLocalTick,
};


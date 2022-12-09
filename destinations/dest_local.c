#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>

#include <json.h>

#include "dest.h"

#define MAX_FILEPATH_LEN 1024

#define MAX_ACTION_NAME_LEN 64
#define MAX_ACTION_LEN (1024 * 1024)

static char* repositoryJsonFilePath;
static char* repositoryActionsPath;
static ActionAddedCallback cachedActionAddedCallback;

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
			fprintf(stderr, "addAction: malloc(): %s\n", strerror(errno));
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
		fprintf(stderr, "destLocalInit: malloc(): %s\n", strerror(errno));

		return 1;
	}
	repositoryActionsPath = malloc(MAX_FILEPATH_LEN);
	if (repositoryActionsPath == NULL) {
		fprintf(stderr, "destLocalInit: malloc(): %s\n", strerror(errno));

		free(repositoryJsonFilePath);
		return 2;
	}


	snprintf(repositoryJsonFilePath, MAX_FILEPATH_LEN, "%s/repository.json", repository);
	snprintf(repositoryActionsPath, MAX_FILEPATH_LEN, "%s/actions", repository);

	// open repository.json
	FILE* file = fopen(repositoryJsonFilePath, "r");
	if (file == NULL) {
		fprintf(stderr, "destLocalInit: fopen(): %s\n", strerror(errno));

		free(repositoryJsonFilePath);
		free(repositoryActionsPath);
		return 3;
	}
	fclose(file);

	return 0;

}

void destLocalShutdown()
{
	if (repositoryJsonFilePath != NULL) {
		free(repositoryJsonFilePath);
		repositoryJsonFilePath = NULL;
	}
	if (repositoryActionsPath != NULL) {
		free(repositoryActionsPath);
		repositoryActionsPath = NULL;
	}

	freeActions(&handledActions);
}

int destLocalPutStorageFile(char* filename, char *buf, size_t size)
{
	return -1;
}

int destLocalGetStorageFile(char* filename, char *buf, size_t *size)
{
	return -1;
}

int destLocalAddActionFile(char* filename, char *buf, size_t size)
{
	return -1;
}

int destLocalGetRepositoryFile(char *buf, size_t *size)
{
	FILE* file = fopen(repositoryJsonFilePath, "r");
	if (file == NULL) {
		fprintf(stderr, "destLocalInit: fopen(): %s\n", strerror(errno));

		return 1;
	}

	int bytesRead = 0;
	while (!feof(file) && bytesRead < *size) {
		bytesRead += fread(buf + bytesRead, 1, *size - bytesRead, file);
	}
	fclose(file);

	if (bytesRead >= *size) {
		fprintf(stderr, "destLocalInit: repository.json file is too large\n");

		return 2;
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
		fprintf(stderr, "warning: destLocalTick(): opendir(): %s\n", strerror(errno));
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
			fprintf(stderr, "warning: destLocalTick(): readdir(): %s\n", strerror(errno));

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

	printf("DEBUG: new actions count: %d\n", newActions.len);

	char* actionFilePath = malloc(MAX_FILEPATH_LEN);
	if (actionFilePath == NULL) {
		fprintf(stderr, "destLocalTick: malloc(): %s\n", strerror(errno));

		return 1;
	}
	char* actionFileBuf = malloc(MAX_ACTION_LEN);
	if (actionFileBuf == NULL) {
		fprintf(stderr, "destLocalTick: malloc(): %s\n", strerror(errno));

		free(actionFilePath);
		return 1;
	}

	for (int i=0; i<newActions.len; i++) {
		printf("DEBUG: handle new action: %s\n", getAction(&newActions, i));

		snprintf(actionFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryActionsPath, getAction(&newActions, i));

		FILE* file = fopen(actionFilePath, "r");
		if (file == NULL) {
			fprintf(stderr, "destLocalInit: fopen(): %s\n", strerror(errno));
			continue;
		}

		size_t bytesRead = 0;
		while (!feof(file) && bytesRead < MAX_ACTION_LEN) {
			bytesRead += fread(actionFileBuf + bytesRead, 1, MAX_ACTION_LEN - bytesRead, file);
		}
		fclose(file);

		if (bytesRead >= MAX_ACTION_LEN) {
			fprintf(stderr, "destLocalInit: action file is too large\n");
			continue;
		}

		actionFileBuf[bytesRead] = 0; // null termination
		if (cachedActionAddedCallback) {
			cachedActionAddedCallback(getAction(&newActions, i), actionFileBuf, bytesRead, newActions.len - i - 1);
		} else {
			fprintf(stderr, "destLocalInit: no action added callback\n");
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
	.putStorageFile = destLocalPutStorageFile,
	.getStorageFile = destLocalGetStorageFile,
	.addActionFile = destLocalAddActionFile,
	.getRepositoryFile = destLocalGetRepositoryFile,
	.setCallbackActionAdded = destLocalSetCallbackActionAdded,
	.isTickable = destLocalIsTickable,
	.tick = destLocalTick,
};


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#include <json.h>

#include "dynarray.h"
#include "filesystem.h"

#include "actions.h"

// TODO: unused fields should not be serialized nor parsed
// -- e.g. content, size, blockSize for removeFile action

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
		newFile->parentDir = containingDir;

		addToDynArray(&containingDir->files, newFile);
		return 0;

	} else if (action->actionType == ActionTypeEditFile) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(action->path, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "doAction: path_split() failed\n");
			return 4;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "doAction: path not found when editing file %s\n", action->path);
			return 5;
		}

		FilesystemFile* file = findFile(containingDir, fileName);
		if (file == NULL) {
			fprintf(stderr, "doAction: file not found: %s\n", action->path);
			return 6;
		}

		file->time = action->time;
		file->content = action->content;
		file->contentLen = action->contentLen;
		file->size = action->size;
		file->blockSize = action->blockSize;
		file->dirtyFlags = 0;
		memset(&file->pendingWrites, 0, sizeof(DynArray));

		return 0;

	} else if (action->actionType == ActionTypeRemoveFile) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(action->path, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "doAction: path_split() failed\n");
			return 7;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "doAction: path not found when removing file %s\n", action->path);
			return 8;
		}

		FilesystemFile* file = findFile(containingDir, fileName);
		if (file == NULL) {
			fprintf(stderr, "doAction: file not found: %s\n", action->path);
			return 9;
		}

		if (removeFromDynArrayUnordered(&containingDir->files, (void*)file) != 0) {
			fprintf(stderr, "doAction: removeFromDynArrayUnordered() failed\n");
			return 10;
		}
		free(file);
		return 0;

	} else if (action->actionType == ActionTypeAddDirectory) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *dirName = path_split(action->path, &pathArray);
		if (dirName == NULL) {
			fprintf(stderr, "doAction: path_split() failed\n");
			return 11;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "doAction: path not found when adding directory %s\n", action->path);
			return 12;
		}

		FilesystemDir* newDir = malloc(sizeof(FilesystemDir));
		if (newDir == NULL) {
			fprintf(stderr, "doAction: malloc(): %s\n", strerror(errno));
			return 13;
		}
		memset(newDir, 0, sizeof(FilesystemDir));

		newDir->name = dirName;
		newDir->time = action->time;
		newDir->parentDir = containingDir;

		addToDynArray(&containingDir->dirs, newDir);
		return 0;

	} else if (action->actionType == ActionTypeRemoveDirectory) {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *dirName = path_split(action->path, &pathArray);
		if (dirName == NULL) {
			fprintf(stderr, "doAction: path_split() failed\n");
			return 14;
		}

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "doAction: path not found when adding directory %s\n", action->path);
			return 15;
		}

		FilesystemDir* dir = findDir(containingDir, dirName);
		if (dir == NULL) {
			fprintf(stderr, "doAction: dir not found: %s\n", action->path);
			return 16;
		}

		if (removeFromDynArrayUnordered(&containingDir->dirs, (void*)dir) != 0) {
			fprintf(stderr, "doAction: removeFromDynArrayUnordered() failed\n");
			return 17;
		}
		freeDynArray(&dir->dirs);
		freeDynArray(&dir->files);
		free(dir);
		return 0;

	} else {
		fprintf(stderr, "doAction: unknown action type: %d\n", action->actionType);
		return -1;
	}

	return -1;
}

static int undoAction(Action* action)
{
	// TODO
	printf("DEBUG: undo action\n");
}

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
		} else if (strcmp(actionTypeStr, "editFile") == 0) {
			actionType = ActionTypeEditFile;
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
}

char* serializeAction(Action* action)
{
	json_object* jsonNewActions = json_object_new_array();
	if (!jsonNewActions) {
		return NULL;
	}
	json_object* jsonNewAction = json_object_new_object();
	if (!jsonNewAction) {
		json_object_put(jsonNewActions);
		return NULL;
	}
	json_object* jsonNewContent = json_object_new_array();
	if (!jsonNewAction) {
		json_object_put(jsonNewActions);
		json_object_put(jsonNewAction);
		return NULL;
	}
	for (int i=0; i<action->contentLen; i++) {
		json_object_array_add(jsonNewContent,
			json_object_new_string(action->content + i*MAX_STORAGE_NAME_LEN));
	}

	json_object_object_add(jsonNewAction,
		"time", json_object_new_int64(action->time));

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
	} else {
		actionStr = "unknown";
	}
	json_object_object_add(jsonNewAction,
		"action", json_object_new_string(actionStr));
	json_object_object_add(jsonNewAction,
		"path", json_object_new_string(action->path));
	json_object_object_add(jsonNewAction,
		"content", jsonNewContent);
	json_object_object_add(jsonNewAction,
		"size", json_object_new_int(action->size));
	json_object_object_add(jsonNewAction,
		"blockSize", json_object_new_int(action->blockSize));

	json_object_array_add(jsonNewActions, jsonNewAction);

	char* jsonData = (char*)json_object_to_json_string_ext(
		jsonNewActions, JSON_C_TO_STRING_PRETTY);

	char* result = malloc(strlen(jsonData)+1);
	if (result == NULL) {
		fprintf(stderr, "serializeAction: malloc(): %s\n", strerror(errno));
		json_object_put(jsonNewActions);
		return NULL;
	}
	memcpy(result, jsonData, strlen(jsonData)+1);

	json_object_put(jsonNewActions);

	return result;
}

void addAction(Action *newAction)
{
	addToDynArray(&actions, newAction);
}

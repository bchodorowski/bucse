#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <json.h>

#include "dest.h"

#define MAX_FILEPATH_LEN 1024

char* repositoryJsonFilePath;

int destLocalInit(char* repository)
{
	// construct file path of repository.json file
	repositoryJsonFilePath = malloc(MAX_FILEPATH_LEN);
	if (repositoryJsonFilePath == NULL)
	{
		fprintf(stderr, "destLocalInit: malloc(): %s\n", strerror(errno));

		return 1;
	}

	snprintf(repositoryJsonFilePath, MAX_FILEPATH_LEN, "%s/repository.json", repository);

	// open repository.json
	FILE* file = fopen(repositoryJsonFilePath, "r");
	if (file == NULL)
	{
		fprintf(stderr, "destLocalInit: fopen(): %s\n", strerror(errno));

		return 2;
	}
	fclose(file);
	return 0;

}

void destLocalShutdown()
{
	if (repositoryJsonFilePath != NULL)
	{
		free(repositoryJsonFilePath);
		repositoryJsonFilePath = NULL;
	}
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
	if (file == NULL)
	{
		fprintf(stderr, "destLocalInit: fopen(): %s\n", strerror(errno));

		return 1;
	}

	int bytesRead = 0;
	while (!feof(file) && bytesRead < *size)
	{
		bytesRead += fread(buf + bytesRead, 1, *size - bytesRead, file);
	}
	fclose(file);

	if (bytesRead >= *size)
	{
		fprintf(stderr, "destLocalInit: repository.json file is too large\n");

		return 2;
	}

	buf[bytesRead] = 0; // null termination
	*size = bytesRead;
	return 0;
}

int destLocalSetCallbackActionAdded(ActionAddedCalback callback)
{
	return -1;
}

int destLocalIsTickable()
{
	return 1;
}

int destLocalTick()
{
	printf("DEBUG: Hello from destLocalTick\n");
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


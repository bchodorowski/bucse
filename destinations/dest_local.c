#include <stddef.h>

#include "dest.h"

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

int destLocalSetCallbackActionAdded(ActionAddedCalback callback)
{
	return -1;
}

Destination destinationLocal = {
	.putStorageFile = destLocalPutStorageFile,
	.getStorageFile = destLocalGetStorageFile,
	.addActionFile = destLocalAddActionFile,
	.setCallbackActionAdded = destLocalSetCallbackActionAdded
};


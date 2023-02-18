#include "dynarray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int addToDynArray(DynArray *dynArray, void* newObject)
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

void freeDynArray(DynArray *dynArray)
{
	if (dynArray->objects != NULL) {
		free(dynArray->objects);
	}
	dynArray->objects = NULL;
	dynArray->len = dynArray->size = 0;
}

int removeFromDynArrayUnordered(DynArray *dynArray, void* element)
{
	for (int i=0; i<dynArray->len; i++) {
		if (dynArray->objects[i] == element) {
			dynArray->objects[i] = dynArray->objects[dynArray->len-1];
			dynArray->len--;
			return 0;
		}
	}
	return 1;
}

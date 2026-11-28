#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include "log.h"

#include "dynarray.h"

// Makes room for count more objects, so that the next count calls of
// addToDynArray() can't fail. Code that changes state that can't be rolled
// back (e.g. writes an action file) reserves first, and adds after.
int reserveDynArray(DynArray *dynArray, int count)
{
	if (count < 0 || dynArray->len > INT_MAX - count) {
		logPrintf(LOG_ERROR, "reserveDynArray: too many objects\n");
		return 1;
	}
	int needed = dynArray->len + count;
	if (needed <= dynArray->size) {
		return 0;
	}

	int newSize = dynArray->size == 0 ? 16 : dynArray->size;
	while (newSize < needed) {
		newSize = newSize > INT_MAX / 2 ? needed : newSize * 2;
	}

	void** newObjects = realloc(dynArray->objects, (size_t)newSize * sizeof(void*));
	if (newObjects == NULL) {
		logPrintf(LOG_ERROR, "reserveDynArray: realloc(): %s\n", strerror(errno));
		return 1;
	}
	dynArray->objects = newObjects;
	dynArray->size = newSize;
	return 0;
}

int addToDynArray(DynArray *dynArray, void* newObject)
{
	if (reserveDynArray(dynArray, 1) != 0) {
		return 1;
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
			if (i != dynArray->len-1)
				dynArray->objects[i] = dynArray->objects[dynArray->len-1];
			dynArray->len--;
			return 0;
		}
	}
	return 1;
}

void removeFromDynArrayUnorderedByIndex(DynArray *dynArray, int i)
{
	if (i != dynArray->len-1)
		dynArray->objects[i] = dynArray->objects[dynArray->len-1];
	dynArray->len--;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#include "dynarray.h"

#include "filesystem.h"

FilesystemDir* root;

FilesystemFile* findFile(FilesystemDir* dir, const char* fileName)
{
	for (int i=0; i<dir->files.len; i++) {
		FilesystemFile* f = dir->files.objects[i];
		if (strcmp(f->name, fileName) == 0) {
			return f;
		}
	}

	return NULL;
}

FilesystemDir* findDir(FilesystemDir* dir, const char* dirName)
{
	for (int i=0; i<dir->dirs.len; i++) {
		FilesystemDir* d = dir->dirs.objects[i];
		if (strcmp(d->name, dirName) == 0) {
			return d;
		}
	}
}

FilesystemDir* findContainingDir(DynArray *pathArray)
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

FilesystemDir* findDirByPath(DynArray *pathArray)
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

const char* path_split(const char* path, DynArray *result)
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

void path_free(DynArray *pathArray)
{
	if (pathArray->len > 0 && pathArray->objects[0] != NULL) {
		free(pathArray->objects[0]);
	}
	freeDynArray(pathArray);
}

char* path_getFilename(DynArray *pathArray)
{
	return pathArray->objects[pathArray->len-1];
}

void path_debugPrint(DynArray *pathArray)
{
	fprintf(stderr, "DEBUG: print debug path:\n");
	for (int i=0; i<pathArray->len; i++) {
		fprintf(stderr, "\t%d: %s\n", i, pathArray->objects[i]);
	}
}

static int getFullFilePathRecursion(char* result, int index, FilesystemDir* dir)
{
	if (dir->parentDir) {
		index = getFullFilePathRecursion(result, index, dir->parentDir);
	}
	if (dir->name) {
		sprintf(result+index, "%s/", dir->name);
		index += strlen(dir->name) + 1;
	}
	return index;
}

char* getFullFilePath(FilesystemFile* file)
{
	int len = strlen(file->name) + 1;
	FilesystemDir *current = file->parentDir;
	while (current) {
		if (current->name) {
			len += 1 + strlen(current->name);
		}
		current = current->parentDir;
	}

	char* result = malloc(len);
	if (result == NULL) {
		fprintf(stderr, "getFullFilePath: malloc(): %s\n", strerror(errno));
		return NULL;
	}

	int index = 0;
	index = getFullFilePathRecursion(result, index, file->parentDir);
	sprintf(result+index, "%s", file->name);

	return result;
}


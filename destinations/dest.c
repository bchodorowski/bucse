/*
 * destinations/dest.c
 *
 * Implementation of auxiliary functions used by destination implementations.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "dest.h"

extern Destination destinationLocal;
extern Destination destinationSsh;

int getRandomStorageFileName(char* filename)
{
	FILE* f = fopen("/dev/urandom", "rb");
	if (f == NULL) {
		fprintf(stderr, "getRandomStorageFileName: fopen(): %s\n", strerror(errno));
		filename[0] = 0;
		return 1;
	}

	unsigned char buf[20];
	size_t got = fread(buf, 20, 1, f);
	if (got == 0) {
		fprintf(stderr, "getRandomStorageFileName: fread failed\n");
		fclose(f);
		return 2;
	}

	fclose(f);

	for (int i=0; i<20; i++) {
		sprintf(filename + 2*i, "%02x", buf[i]);
	}

	return 0;
}

void getDestinationBasedOnPathPrefix(Destination** destPtr,
	char** realPathPtr,
	char* path)
{
	if (strncmp(path, "file://", 7) == 0) {
		(*realPathPtr) = realpath(path + 7, NULL);
		(*destPtr) = &destinationLocal;
	} else if (strncmp(path, "ssh://", 6) == 0) {
		(*realPathPtr) = strdup(path + 6);
		(*destPtr) = &destinationSsh;
	} else {
		(*realPathPtr) = realpath(path, NULL);
		(*destPtr) = &destinationLocal;
	}

}

/*
 * destinations/dest.c
 *
 * Implementation of auxiliary functions used by destination implementations.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "dest.h"

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


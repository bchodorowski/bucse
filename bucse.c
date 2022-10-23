#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <fuse.h>

#include "destinations/dest.h"
#include "encryption/encr.h"

static char TESTCONTENT[] = "Wololo\n";
static char TESTFILENAME[] = "test";

static int bucse_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0)
	{
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	}
	else if (strcmp(path+1, TESTFILENAME) == 0)
	{
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = strlen(TESTCONTENT);
	}
	else
		res = -ENOENT;

	return res;
}

static int bucse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, TESTFILENAME, NULL, 0);

	return 0;
}

static int bucse_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path+1, TESTFILENAME) != 0)
		return -ENOENT;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int bucse_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	size_t len;
	(void) fi;
	if(strcmp(path+1, TESTFILENAME) != 0)
		return -ENOENT;

	len = strlen(TESTCONTENT);
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, TESTCONTENT + offset, size);
	} else
		size = 0;

	return size;
}

struct fuse_operations bb_oper = {
	.getattr = bucse_getattr,
	.open = bucse_open,
	.read = bucse_read,
	.readdir = bucse_readdir,
};

extern Destination destinationLocal;
extern Encryption encryptionNone;

int main(int argc, char** argv)
{
	Destination *destination;
	destination = &destinationLocal;

	printf("Destination interface test: %d\n", destination->addActionFile(NULL, NULL, 0));

	Encryption *encryption;
	encryption = &encryptionNone;

	printf("Encryption interface test: %d\n", encryption->encrypt(NULL, 0, NULL, NULL, NULL));

	int fuse_stat;
	fuse_stat = fuse_main(argc, argv, &bb_oper, NULL);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
	return fuse_stat;
}

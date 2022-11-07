#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <fuse.h>
#include <json.h>

#include "destinations/dest.h"
#include "encryption/encr.h"

#define PACKAGE_VERSION "current"

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

struct fuse_operations bucse_oper = {
	.getattr = bucse_getattr,
	.open = bucse_open,
	.read = bucse_read,
	.readdir = bucse_readdir,
};

struct bucse_config {
	char *repository;
};
enum {
	KEY_HELP,
	KEY_VERSION,
};

#define BUCSE_OPT(t, p, v) { t, offsetof(struct bucse_config, p), v }

static struct fuse_opt bucse_opts[] = {
	BUCSE_OPT("repository=%s", repository, 0),
	BUCSE_OPT("-r %s", repository, 0),

	FUSE_OPT_KEY("-V",             KEY_VERSION),
	FUSE_OPT_KEY("--version",      KEY_VERSION),
	FUSE_OPT_KEY("-h",             KEY_HELP),
	FUSE_OPT_KEY("--help",         KEY_HELP),
	FUSE_OPT_END
};

static int bucse_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	switch (key) {
	case KEY_HELP:
		fuse_opt_add_arg(outargs, "-h");
		fuse_main(outargs->argc, outargs->argv, &bucse_oper, NULL);
		fprintf(stderr,
				"\n"
				"bucse options:\n"
				"    -o repository=STRING   TODO\n"
				"    -r STRING              same as '-orepository=STRING'\n"
				, outargs->argv[0]);
		exit(1);

	case KEY_VERSION:
		fprintf(stderr, "bucse version %s\n", PACKAGE_VERSION);
		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &bucse_oper, NULL);
		exit(0);
	}
	return 1;
}

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

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct bucse_config conf;

	memset(&conf, 0, sizeof(conf));

	fuse_opt_parse(&args, &conf, bucse_opts, bucse_opt_proc);

	printf("Repository: %s\n", conf.repository);

	json_object *repositoryJson;

	int fuse_stat;
	fuse_stat = fuse_main(args.argc, args.argv, &bucse_oper, NULL);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
	return fuse_stat;
}

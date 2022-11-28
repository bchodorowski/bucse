#define FUSE_USE_VERSION 34

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <fuse_lowlevel.h>
#include <fuse.h>

#include <json.h>

#include <pthread.h>

#include "destinations/dest.h"
#include "encryption/encr.h"

#define PACKAGE_VERSION "current"

static char TESTCONTENT[] = "Wololo\n";
static char TESTFILENAME[] = "test";

static int bucse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	(void) fi;
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
		off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	(void) offset;
	(void) fi;
	(void) flags;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	filler(buf, TESTFILENAME, NULL, 0, 0);

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

static Destination *destination;
static Encryption *encryption;
static pthread_t tickThread;

static pthread_mutex_t shutdownMutex;
static int shutdownTicking = 0;

void* tickThreadFunc(void* param)
{
	printf("DEBUG: Hello from tickThreadFunc\n");
	for (;;)
	{
		pthread_mutex_lock(&shutdownMutex);
		if (shutdownTicking)
		{
			pthread_mutex_unlock(&shutdownMutex);
			break;
		}
		pthread_mutex_unlock(&shutdownMutex);

		if (destination->tick() != 0)
		{
			break;
		}
		sleep(1);
	}
	return 0;
}

int bucse_fuse_main(int argc, char *argv[], const struct fuse_operations *op,
		size_t op_size, void *user_data)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse *fuse;
	struct fuse_cmdline_opts opts;
	int res;
	struct fuse_loop_config config;

	if (fuse_parse_cmdline(&args, &opts) != 0)
		return 1;

	if (opts.show_version) {
		printf("FUSE library version %s\n", PACKAGE_VERSION);
		fuse_lowlevel_version();
		res = 0;
		goto out1;
	}

	if (opts.show_help) {
		if(args.argv[0][0] != '\0')
			printf("usage: %s [options] <mountpoint>\n\n",
					args.argv[0]);
		printf("FUSE options:\n");
		fuse_cmdline_help();
		fuse_lib_help(&args);
		res = 0;
		goto out1;
	}

	if (!opts.show_help &&
			!opts.mountpoint) {
		fuse_log(FUSE_LOG_ERR, "error: no mountpoint specified\n");
		res = 2;
		goto out1;
	}

	fuse = fuse_new(&args, op, op_size, user_data);
	if (fuse == NULL) {
		res = 3;
		goto out1;
	}

	if (fuse_mount(fuse,opts.mountpoint) != 0) {
		res = 4;
		goto out2;
	}

	// TODO: switch daemonize with an argument[?]
	/*
	if (fuse_daemonize(opts.foreground) != 0) {
		res = 5;
		goto out3;
	}
	*/

	// initialize destination thread
	if (destination->isTickable())
	{
		int ret = pthread_create(&tickThread, NULL, tickThreadFunc, NULL);
	}

	struct fuse_session *se = fuse_get_session(fuse);
	if (fuse_set_signal_handlers(se) != 0) {
		res = 6;
		goto out3;
	}

	if (opts.singlethread)
		res = fuse_loop(fuse);
	else {
		config.clone_fd = opts.clone_fd;
		config.max_idle_threads = opts.max_idle_threads;
		res = fuse_session_loop_mt(se, &config);
	}
	if (res)
		res = 8;

	fuse_remove_signal_handlers(se);
out3:
	fuse_unmount(fuse);
out2:
	fuse_destroy(fuse);
out1:
	free(opts.mountpoint);
	fuse_opt_free_args(&args);
	return res;
}

void actionAdded(char* actionName, char* buf, size_t size, int moreInThisBatch)
{
	printf("%s\n  %s\n  %d\n  %d\n", actionName, buf, size, moreInThisBatch);
}

#define MAX_REPOSITORY_JSON_LEN (1024 * 1024)

int main(int argc, char** argv)
{
	destination = &destinationLocal;

	destination->setCallbackActionAdded(&actionAdded);

	encryption = &encryptionNone;


	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct bucse_config conf;

	memset(&conf, 0, sizeof(conf));

	fuse_opt_parse(&args, &conf, bucse_opts, bucse_opt_proc);

	printf("Repository: %s\n", conf.repository);
	int err = destination->init(conf.repository);
	if (err != 0)
	{
		fprintf(stderr, "destination->init(): %d\n", err);
		return 1;
	}

	char* repositoryJsonFileContents = malloc(MAX_REPOSITORY_JSON_LEN);
	if (repositoryJsonFileContents == NULL)
	{
		fprintf(stderr, "malloc(): %s\n", strerror(errno));
		return 2;
	}
	size_t repositoryJsonFileLen = MAX_REPOSITORY_JSON_LEN;

	err = destination->getRepositoryFile(repositoryJsonFileContents, &repositoryJsonFileLen);
	if (err != 0)
	{
		fprintf(stderr, "destination->getRepositoryFile(): %d\n", err);

		free(repositoryJsonFileContents);
		return 3;
	}

	json_tokener* tokener = json_tokener_new();
	json_object* repositoryJson = json_tokener_parse_ex(tokener, repositoryJsonFileContents, repositoryJsonFileLen);

	free(repositoryJsonFileContents);
	json_tokener_free(tokener);

	if (repositoryJson == NULL)
	{
		fprintf(stderr, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		return 4;
	}

	printf("DEBUG: type: %s\n", json_type_to_name(
				json_object_get_type(repositoryJson)));

	json_object* encryptionField;
	if (json_object_object_get_ex(repositoryJson, "encryption", &encryptionField) == 0)
	{
		fprintf(stderr, "repository object doesn't have 'encryption' field\n");
		json_object_put(repositoryJson);
		return 5;
	}

	if (json_object_get_type(encryptionField) != json_type_string)
	{
		fprintf(stderr, "'encryption' field is not a string\n");
		json_object_put(repositoryJson);
		return 6;
	}

	const char* encryptionFieldStr = json_object_get_string(encryptionField);

	printf("DEBUG: enc: %s\n", encryptionFieldStr);
	if (strcmp(encryptionFieldStr, "none") == 0)
	{
		printf("DEBUG: encryption none\n");
	}
	else
	{
		fprintf(stderr, "Unsupported encryption: %s\n", encryptionFieldStr);
		json_object_put(repositoryJson);
		return 6;
	}

	json_object_put(repositoryJson);

	int fuse_stat;
	fuse_stat = bucse_fuse_main(args.argc, args.argv, &bucse_oper, sizeof(bucse_oper), NULL);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
	pthread_mutex_lock(&shutdownMutex);
	shutdownTicking = 1;
	pthread_mutex_unlock(&shutdownMutex);

	int ret = pthread_join(tickThread, NULL);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_join: %d\n", ret);
	}
	destination->shutdown();

	return fuse_stat;
}

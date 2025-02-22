/*
 * bucse-mount.c
 *
 * Main file of the fuse application used to mount bucse filesystems.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <termios.h>

#include <fuse_lowlevel.h>
#include <fuse.h>

#include <json.h>

#include <pthread.h>

#include "dynarray.h"
#include "filesystem.h"
#include "actions.h"

#include "conf.h"
#include "log.h"
#include "cache.h"

#include "destinations/dest.h"
#include "encryption/encr.h"

#include "operations/operations.h" // TODO: remove?
#include "operations/getattr.h"
#include "operations/flush.h"
#include "operations/readdir.h"
#include "operations/open.h"
#include "operations/create.h"
#include "operations/release.h"
#include "operations/read.h"
#include "operations/write.h"
#include "operations/unlink.h"
#include "operations/mkdir.h"
#include "operations/rmdir.h"
#include "operations/truncate.h"
#include "operations/rename.h"
#include "operations/init.h"

#define PACKAGE_VERSION "current"

uid_t cachedUid;
gid_t cachedGid;

static void recursivelyFreeFilesystem(FilesystemDir* dir) {
	for (int i=0; i<dir->dirs.len; i++) {
		recursivelyFreeFilesystem(dir->dirs.objects[i]);
	}
	for (int i=0; i<dir->files.len; i++) {
		flushFile((FilesystemFile*)dir->files.objects[i]);
		free(dir->files.objects[i]);
	}
	
	freeDynArray(&dir->dirs);
	freeDynArray(&dir->files);
	free(dir);
}

extern Destination destinationLocal;
extern Destination destinationSsh;
extern Encryption encryptionNone;
extern Encryption encryptionAes;

Destination *destination;
Encryption *encryption;
static pthread_t tickThread;

static pthread_mutex_t shutdownMutex;
static int shutdownTicking = 0;

static void actionAddedDecrypt(char* actionName, char* buf, size_t size, int moreInThisBatch)
{
	size_t decryptedBufLen = MAX_ACTION_LEN + DECRYPTED_BUFFER_MARGIN;
	char* decryptedBuf = malloc(decryptedBufLen);
	if (decryptedBuf == NULL) {
		logPrintf(LOG_ERROR, "decryptAndAddActionFile: malloc(): %s\n", strerror(errno));
		return;
	}

	int result = encryption->decrypt(buf, size,
		decryptedBuf, &decryptedBufLen,
		conf.passphrase);
	
	if (result != 0) {
		logPrintf(LOG_ERROR, "actionAddedDecrypt: decrypt failed: %d\n", result);
		free(decryptedBuf);
		return;
	}
	actionAdded(actionName, decryptedBuf, decryptedBufLen, moreInThisBatch);
	free(decryptedBuf);
}

struct fuse_operations bucse_oper = {
	.getattr = bucse_getattr_guarded,
	.open = bucse_open_guarded,
	.create = bucse_create_guarded,
	.release = bucse_release_guarded,
	.read = bucse_read_guarded,
	.readdir = bucse_readdir_guarded,
	.write = bucse_write_guarded,
	.unlink = bucse_unlink_guarded,
	.mkdir = bucse_mkdir_guarded,
	.rmdir = bucse_rmdir_guarded,
	.truncate = bucse_truncate_guarded,
	.rename = bucse_rename_guarded,
	.flush = bucse_flush_guarded,
	.init = bucse_init_guarded,
};

enum {
	KEY_HELP,
	KEY_VERSION,
};

#define BUCSE_OPT(t, p, v) { t, offsetof(struct bucse_config, p), v }

static struct fuse_opt bucse_opts[] = {
	BUCSE_OPT("repository=%s", repository, 0),
	BUCSE_OPT("-r %s", repository, 0),
	BUCSE_OPT("verbose=%d", verbose, 0),
	BUCSE_OPT("-v %d", verbose, 0),
	BUCSE_OPT("passphrase=%s", passphrase, 0),
	BUCSE_OPT("-p %s", passphrase, 0),

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
		fprintf(stdout,
				"\n"
				"bucse options:\n"
				"    -o repository=STRING   TODO\n"
				"    -r STRING              same as '-orepository=STRING'\n"
				"    -o verbose=INTEGER     TODO\n"
				"    -v INTEGER             same as '-overbose=INTEGER'\n"
				"    -o passphrase=STRING   TODO\n"
				"    -p STRING              same as '-opassphrase=STRING'\n");
		exit(1);

	case KEY_VERSION:
		fprintf(stdout, "bucse version %s\n", PACKAGE_VERSION);
		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &bucse_oper, NULL);
		exit(0);
	}
	return 1;
}

void* tickThreadFunc(void* param)
{
	for (;;) {
		pthread_mutex_lock(&shutdownMutex);
		if (shutdownTicking) {
			pthread_mutex_unlock(&shutdownMutex);
			break;
		}
		pthread_mutex_unlock(&shutdownMutex);

		pthread_mutex_lock(&bucseMutex);
		int tickResult = destination->tick();
		pthread_mutex_unlock(&bucseMutex);

		if (tickResult != 0) {
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
		fuse_log(FUSE_LOG_ERR, "no mountpoint specified\n");
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

	if (fuse_daemonize(opts.foreground) != 0) {
		res = 5;
		goto out3;
	}

	// initialize destination thread
	if (destination->isTickable())
	{
		int ret = pthread_create(&tickThread, NULL, tickThreadFunc, NULL);
		if (ret != 0) {
			fuse_log(FUSE_LOG_ERR, "bucse_fuse_main: pthread_create: %d\n", ret);
			res = 6;
			goto out3;
		}
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

#define MAX_REPOSITORY_JSON_LEN (1024 * 1024)
int parseRepositoryJsonFile() {
	char* repositoryJsonFileContents = malloc(MAX_REPOSITORY_JSON_LEN);
	if (repositoryJsonFileContents == NULL)
	{
		logPrintf(LOG_ERROR, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	size_t repositoryJsonFileLen = MAX_REPOSITORY_JSON_LEN;

	int err = destination->getRepositoryJsonFile(repositoryJsonFileContents, &repositoryJsonFileLen);
	if (err != 0)
	{
		logPrintf(LOG_ERROR, "destination->getRepositoryJsonFile(): %d\n", err);

		free(repositoryJsonFileContents);
		return 2;
	}

	json_tokener* tokener = json_tokener_new();
	json_object* repositoryJson = json_tokener_parse_ex(tokener, repositoryJsonFileContents, repositoryJsonFileLen);

	free(repositoryJsonFileContents);
	json_tokener_free(tokener);

	if (repositoryJson == NULL)
	{
		logPrintf(LOG_ERROR, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		return 3;
	}

	json_object* encryptionField;
	if (json_object_object_get_ex(repositoryJson, "encryption", &encryptionField) == 0)
	{
		logPrintf(LOG_ERROR, "repository object doesn't have 'encryption' field\n");
		json_object_put(repositoryJson);
		return 4;
	}

	if (json_object_get_type(encryptionField) != json_type_string)
	{
		logPrintf(LOG_ERROR, "'encryption' field is not a string\n");
		json_object_put(repositoryJson);
		return 5;
	}

	const char* encryptionFieldStr = json_object_get_string(encryptionField);

	if (strcmp(encryptionFieldStr, "none") == 0) {
		encryption = &encryptionNone;
	} else if (strcmp(encryptionFieldStr, "aes") == 0) {
		encryption = &encryptionAes;
	} else {
		logPrintf(LOG_ERROR, "Unsupported encryption: %s\n", encryptionFieldStr);
		json_object_put(repositoryJson);
		return 6;
	}

	json_object_put(repositoryJson);
	return 0;
}

#define MAX_REPOSITORY_LEN (1024 * 1024)
int parseRepositoryFile() {
	char* repositoryFileContents = malloc(MAX_REPOSITORY_LEN);
	if (repositoryFileContents == NULL)
	{
		logPrintf(LOG_ERROR, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	size_t repositoryFileLen = MAX_REPOSITORY_LEN;

	int err = destination->getRepositoryFile(repositoryFileContents, &repositoryFileLen);
	if (err != 0)
	{
		logPrintf(LOG_ERROR, "destination->getRepositoryFile(): %d\n", err);

		free(repositoryFileContents);
		return 2;
	}

	size_t decryptedBufLen = MAX_REPOSITORY_LEN + DECRYPTED_BUFFER_MARGIN;
	char* decryptedBuf = malloc(decryptedBufLen);
	if (decryptedBuf == NULL) {
		logPrintf(LOG_ERROR, "parseRepositoryFile: malloc(): %s\n", strerror(errno));

		free(repositoryFileContents);
		return 3;
	}

	int result = encryption->decrypt(repositoryFileContents, repositoryFileLen,
		decryptedBuf, &decryptedBufLen,
		conf.passphrase);
	
	if (result != 0) {
		logPrintf(LOG_ERROR, "parseRepositoryFile: decrypt failed: %d\n", result);

		free(repositoryFileContents);
		free(decryptedBuf);
		return 4;
	}

	json_tokener* tokener = json_tokener_new();
	json_object* repository = json_tokener_parse_ex(tokener, decryptedBuf, decryptedBufLen);

	free(repositoryFileContents);
	free(decryptedBuf);
	json_tokener_free(tokener);

	if (repository == NULL)
	{
		logPrintf(LOG_ERROR, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		return 5;
	}

	json_object* timeField;
	if (json_object_object_get_ex(repository, "time", &timeField) == 0)
	{
		logPrintf(LOG_ERROR, "repository object doesn't have 'time' field\n");
		json_object_put(repository);
		return 6;
	}

	if (json_object_get_type(timeField) != json_type_int)
	{
		logPrintf(LOG_ERROR, "'time' field is not a string\n");
		json_object_put(repository);
		return 7;
	}
	int64_t time = json_object_get_int64(timeField);

	root->mtime = root->atime = time;

	json_object_put(repository);
	return 0;
}

int main(int argc, char** argv)
{
	cachedUid = geteuid();
	cachedGid = getegid();

	root = malloc(sizeof(FilesystemDir));
	if (root == NULL) {
		logPrintf(LOG_ERROR, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	memset(root, 0, sizeof(FilesystemDir));

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	confInit();

	fuse_opt_parse(&args, &conf, bucse_opts, bucse_opt_proc);

	if (conf.repository == NULL) {
		logPrintf(LOG_ERROR, "no repository specified\n");
		return 2;
	}
	if (cacheInit() != 0) {
		logPrintf(LOG_ERROR, "cache initialization failed\n");
		confCleanup();
		return 3;
	}

	getDestinationByPathPrefix(&destination,
		&conf.repositoryRealPath, conf.repository);
	int err = destination->init(conf.repositoryRealPath);

	if (err != 0)
	{
		logPrintf(LOG_ERROR, "destination->init(): %d\n", err);
		cacheCleanup();
		recursivelyFreeFilesystem(root);
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 4;
	}
	destination->setCallbackActionAdded(&actionAddedDecrypt);

	if (parseRepositoryJsonFile() != 0) {
		logPrintf(LOG_ERROR, "parseRepositoryJsonFile() failed\n");

		destination->shutdown();
		cacheCleanup();
		recursivelyFreeFilesystem(root);
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 5;
	}

	if (encryption->needsPassphrase() && conf.passphrase == NULL) {
		int gotPass = 0;

		if (isatty(fileno(stdin)) && isatty(fileno(stdout))) {
			fprintf(stdout, "Passphrase: ");
			fflush(stdout);

			struct termios term;
			tcgetattr(fileno(stdin), &term);

			term.c_lflag &= ~ECHO;
			tcsetattr(fileno(stdin), 0, &term);

			char passwd[1024];
			memset(passwd, 0, 1024);
			fgets(passwd, sizeof(passwd), stdin);

			term.c_lflag |= ECHO;
			tcsetattr(fileno(stdin), 0, &term);
			fprintf(stdout, "\n");

			passwd[1024 - 1] = 0;
			if (strlen(passwd) > 0 && passwd[strlen(passwd)-1] == '\n') {
				passwd[strlen(passwd)-1] = 0;
			}
			if (strlen(passwd) > 0) {
				conf.passphrase = malloc(strlen(passwd) + 1);
				if (conf.passphrase == NULL) {
					logPrintf(LOG_ERROR, "malloc() failed\n");
				} else {
					memcpy(conf.passphrase, passwd, strlen(passwd)+1);
					gotPass = 1;
				}
			}
		}

		if (gotPass == 0) {
			logPrintf(LOG_ERROR, "Encryption needs a passphrase\n");

			destination->shutdown();
			cacheCleanup();
			recursivelyFreeFilesystem(root);
			actionsCleanup();
			fuse_opt_free_args(&args);
			confCleanup();
			return 6;
		}
	}

	err = parseRepositoryFile();
	if (err != 0) {
		logPrintf(LOG_ERROR, "parseRepositoryFile() failed\n");
		if (err == 4) {
			logPrintf(LOG_ERROR, "Is the passphrase correct?\n");
		}

		destination->shutdown();
		cacheCleanup();
		recursivelyFreeFilesystem(root);
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 7;
	}

	int fuse_stat;
	fuse_stat = bucse_fuse_main(args.argc, args.argv, &bucse_oper, sizeof(bucse_oper), NULL);
	logPrintf(LOG_DEBUG, "fuse_main returned %d\n", fuse_stat);
	pthread_mutex_lock(&shutdownMutex);
	shutdownTicking = 1;
	pthread_mutex_unlock(&shutdownMutex);

	int ret = pthread_join(tickThread, NULL);
	if (ret != 0)
	{
		logPrintf(LOG_ERROR, "pthread_join: %d\n", ret);
	}
	destination->shutdown();

	cacheCleanup();
	// free filesystem
	recursivelyFreeFilesystem(root);

	actionsCleanup();

	fuse_opt_free_args(&args);
	confCleanup();
	return fuse_stat;
}

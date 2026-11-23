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
#include "tar.h"

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
// set only once tickThread was created, so that it is joined only then
static int tickThreadStarted = 0;

static pthread_mutex_t shutdownMutex;
static int shutdownTicking = 0;

static int actionAddedDecryptOneAction(char* actionName, char* buf, size_t size)
{
	size_t decryptedBufLen = MAX_ACTION_LEN + DECRYPTED_BUFFER_MARGIN;
	char* decryptedBuf = malloc(decryptedBufLen);
	if (decryptedBuf == NULL) {
		logPrintf(LOG_ERROR, "actionAddedDecryptOneAction: malloc(): %s\n", strerror(errno));
		return 1;
	}

	int result = encryption->decrypt(buf, size,
		decryptedBuf, &decryptedBufLen,
		conf.passphrase);
	
	if (result != 0) {
		logPrintf(LOG_ERROR, "actionAddedDecryptOneAction: decrypt failed: %d\n", result);
		free(decryptedBuf);
		return 2;
	}
	result = actionAdded(actionName, decryptedBuf, decryptedBufLen);
	free(decryptedBuf);
	return result;
}

static int endsWithTar(const char* buf) {
    size_t lenBuf = strlen(buf);

    if (lenBuf < 4) {
        return 0;
    }
    // Compare the end of buf to suffix
    return strcmp(buf + lenBuf - 4, ".tar") == 0;
}

static int actionAddedDecrypt(char* actionName, char* buf, size_t size)
{
	if (endsWithTar(actionName)) {
		int result = forEveryFileInTar(buf, size, actionAddedDecryptOneAction);
		if (result != 0) {
			logPrintf(LOG_ERROR, "actionAddedDecrypt: tar file handling failed: %d\n", result);
		}
		return result;
	} else {
		return actionAddedDecryptOneAction(actionName, buf, size);
	}
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
	BUCSE_OPT("ro", readOnly, 1),
	BUCSE_OPT("-R", readOnly, 1),
	BUCSE_OPT("--read_only", readOnly, 1),
	BUCSE_OPT("check_interval=%d", checkInterval, 0),
	BUCSE_OPT("-i %d", checkInterval, 0),

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
				"    -o repository=STRING   target repository address\n"
				"                           can be a directory for local repositories\n"
				"                           use URL starting with ssh:// for ssh repositories\n"
				"    -r STRING              same as '-orepository=STRING'\n"
				"    -o verbose=INTEGER     verbosity level (default: 2)\n"
				"                           0 -- errors\n"
				"                           1 -- all the above + warnings\n"
				"                           2 -- all the above + notes\n"
				"                           3 -- all the above + debug messages\n"
				"                           4 -- all the above + verbose debug messages\n"
				"    -v INTEGER             same as '-overbose=INTEGER'\n"
				"    -o passphrase=STRING   target repository passphrase\n"
				"    -p STRING              same as '-opassphrase=STRING'\n"
				"    -o ro                  read only mode\n"
				"    -R                     same as '-oro'\n"
				"    --read_only            same as '-oro'\n"
				"    -o check_interval=INTEGER\n"
				"                           how often to list the repository's action\n"
				"                           files to find new ones, in seconds (default: 10);\n"
				"                           a destination may also learn about new files\n"
				"                           in between in its own way\n"
				"    -i INTEGER             same as '-ocheck_interval=INTEGER'\n");
		exit(0);

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

	// -h/--help and -V/--version never get here: bucse_opt_proc() handles
	// them (with libfuse's fuse_main()) and exits
	if (fuse_parse_cmdline(&args, &opts) != 0)
		return 1;

	if (!opts.mountpoint) {
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

	
	// call postInit()
	{
		pthread_mutex_lock(&bucseMutex);
		int tickResult = destination->postInit();
		pthread_mutex_unlock(&bucseMutex);
		if (tickResult != 0) {
			res = 5;
			goto out3;
		}
	}

	if (fuse_daemonize(opts.foreground) != 0) {
		res = 6;
		goto out3;
	}

	// initialize destination thread
	if (destination->isTickable()) {
		int ret = pthread_create(&tickThread, NULL, tickThreadFunc, NULL);
		if (ret != 0) {
			fuse_log(FUSE_LOG_ERR, "bucse_fuse_main: pthread_create: %d\n", ret);
			res = 7;
			goto out3;
		}
		tickThreadStarted = 1;
	}

	struct fuse_session *se = fuse_get_session(fuse);
	if (fuse_set_signal_handlers(se) != 0) {
		res = 8;
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
		res = 9;

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
	if (repositoryJsonFileContents == NULL) {
		logPrintf(LOG_ERROR, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	size_t repositoryJsonFileLen = MAX_REPOSITORY_JSON_LEN;

	int err = destination->getRepositoryJsonFile(repositoryJsonFileContents, &repositoryJsonFileLen);
	if (err != 0) {
		logPrintf(LOG_ERROR, "destination->getRepositoryJsonFile(): %d\n", err);

		free(repositoryJsonFileContents);
		return 2;
	}

	json_tokener* tokener = json_tokener_new();
	json_object* repositoryJson = json_tokener_parse_ex(tokener, repositoryJsonFileContents, repositoryJsonFileLen);

	free(repositoryJsonFileContents);

	if (repositoryJson == NULL) {
		logPrintf(LOG_ERROR, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		json_tokener_free(tokener);
		return 3;
	}
	json_tokener_free(tokener);

	json_object* encryptionField;
	if (json_object_object_get_ex(repositoryJson, "encryption", &encryptionField) == 0) {
		logPrintf(LOG_ERROR, "repository object doesn't have 'encryption' field\n");
		json_object_put(repositoryJson);
		return 4;
	}

	if (json_object_get_type(encryptionField) != json_type_string) {
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
	if (repositoryFileContents == NULL) {
		logPrintf(LOG_ERROR, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	size_t repositoryFileLen = MAX_REPOSITORY_LEN;

	int err = destination->getRepositoryFile(repositoryFileContents, &repositoryFileLen);
	if (err != 0) {
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

	if (repository == NULL) {
		logPrintf(LOG_ERROR, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		json_tokener_free(tokener);
		return 5;
	}
	json_tokener_free(tokener);

	json_object* timeField;
	if (json_object_object_get_ex(repository, "time", &timeField) == 0) {
		logPrintf(LOG_ERROR, "repository object doesn't have 'time' field\n");
		json_object_put(repository);
		return 6;
	}

	if (json_object_get_type(timeField) != json_type_int) {
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
		recursivelyFreeFilesystem(root);
		fuse_opt_free_args(&args);
		confCleanup();
		return 2;
	}
	if (conf.checkInterval < 1) {
		logPrintf(LOG_ERROR, "check interval must be at least 1 second\n");
		recursivelyFreeFilesystem(root);
		fuse_opt_free_args(&args);
		confCleanup();
		return 2;
	}
	if (cacheInit() != 0) {
		logPrintf(LOG_ERROR, "cache initialization failed\n");
		recursivelyFreeFilesystem(root);
		fuse_opt_free_args(&args);
		confCleanup();
		return 3;
	}

	getDestinationByPathPrefix(&destination,
		&conf.repositoryRealPath, conf.repository);
	int err = destination->init(conf.repositoryRealPath);

	if (err != 0) {
		logPrintf(LOG_ERROR, "destination->init(): %d\n", err);
		cacheCleanup();
		recursivelyFreeFilesystem(root);
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 4;
	}
	destination->setCallbackActionAdded(&actionAddedDecrypt);
	destination->setCallbackFlushActionsAdded(&flushActionsAdded);

	if (parseRepositoryJsonFile() != 0) {
		logPrintf(LOG_ERROR, "parseRepositoryJsonFile() failed\n");

		cacheCleanup();
		recursivelyFreeFilesystem(root);
		destination->shutdown();
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

			// turn off echo, and restore the original settings right
			// after reading, also when reading fails
			struct termios termOrig;
			int termSaved = (tcgetattr(fileno(stdin), &termOrig) == 0);
			if (termSaved) {
				struct termios term = termOrig;
				term.c_lflag &= ~ECHO;
				tcsetattr(fileno(stdin), TCSANOW, &term);
			}

			char passwd[1024];
			memset(passwd, 0, 1024);
			char* fgetsResult = fgets(passwd, sizeof(passwd), stdin);

			if (termSaved) {
				tcsetattr(fileno(stdin), TCSANOW, &termOrig);
			}
			fprintf(stdout, "\n");

			if (fgetsResult == NULL) {
				logPrintf(LOG_ERROR, "fgets() failed\n");

				cacheCleanup();
				recursivelyFreeFilesystem(root);
				destination->shutdown();
				actionsCleanup();
				fuse_opt_free_args(&args);
				confCleanup();
				return 6;
			}

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

			cacheCleanup();
			recursivelyFreeFilesystem(root);
			destination->shutdown();
			actionsCleanup();
			fuse_opt_free_args(&args);
			confCleanup();
			return 7;
		}
	}

	err = parseRepositoryFile();
	if (err != 0) {
		logPrintf(LOG_ERROR, "parseRepositoryFile() failed\n");
		if (err == 4) {
			logPrintf(LOG_ERROR, "Is the passphrase correct?\n");
		}

		cacheCleanup();
		recursivelyFreeFilesystem(root);
		destination->shutdown();
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 8;
	}

	int fuse_stat;
	fuse_stat = bucse_fuse_main(args.argc, args.argv, &bucse_oper, sizeof(bucse_oper), NULL);
	logPrintf(LOG_DEBUG, "fuse_main returned %d\n", fuse_stat);
	pthread_mutex_lock(&shutdownMutex);
	shutdownTicking = 1;
	pthread_mutex_unlock(&shutdownMutex);

	// bucse_fuse_main() may return before creating the thread (e.g. the
	// mount failed), or the destination may not be tickable
	if (tickThreadStarted) {
		int ret = pthread_join(tickThread, NULL);
		if (ret != 0) {
			logPrintf(LOG_ERROR, "pthread_join: %d\n", ret);
		}
	}

	cacheCleanup();
	// free filesystem
	recursivelyFreeFilesystem(root);
	destination->shutdown();

	actionsCleanup();

	fuse_opt_free_args(&args);
	confCleanup();
	return fuse_stat;
}

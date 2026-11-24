/*
 * bucse-init.c
 *
 * The program for creating and initializing a bucse repository.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#include <json.h>

#include "conf.h"
#include "passphrase.h"
#include "log.h"
#include "time.h"

#include "destinations/dest.h"
#include "encryption/encr.h"

Destination *destination;
Encryption *encryption;

static int initRepo(char* repository, char* passphrase, char* encryptionStr,
	char* name, char* comment)
{
	char* realPath = NULL;
	getDestinationByPathPrefix(&destination, &realPath, repository);
	int err = destination->init(realPath);
	free(realPath);
	if (err != 0)
	{
		logPrintf(LOG_ERROR, "destination->init(): %d\n", err);
		return 1;
	}
	err = destination->createDirs();
	if (err != 0)
	{
		logPrintf(LOG_ERROR, "destination->createDirs(): %d\n", err);
		return 2;
	}

	// save new repository.json
	json_object* jsonRepositoryJson = json_object_new_object();
	if (!jsonRepositoryJson) {
		// TODO: remove dir?
		return 3;
	}
	json_object_object_add(jsonRepositoryJson,
		"name", json_object_new_string(name ? name : "unnamed"));
	json_object_object_add(jsonRepositoryJson,
		"comment", json_object_new_string(comment ? comment : ""));
	json_object_object_add(jsonRepositoryJson,
		"encryption", json_object_new_string(
			encryptionStr ? encryptionStr : "none"));

	char* jsonData = (char*)json_object_to_json_string_ext(
		jsonRepositoryJson, JSON_C_TO_STRING_PRETTY);
	if (jsonData == NULL) {
		logPrintf(LOG_ERROR, "initRepo: json_object_to_json_string_ext() failed\n");
		json_object_put(jsonRepositoryJson);
		return 3;
	}

	err = destination->putRepositoryJsonFile(jsonData, strlen(jsonData));
	json_object_put(jsonRepositoryJson);
	if (err != 0) {
		logPrintf(LOG_ERROR, "initRepo: destination->putRepositoryJsonFile(): %d\n", err);
		return 3;
	}

	// save new repository

	json_object* jsonRepository = json_object_new_object();
	if (!jsonRepository) {
		// TODO: remove dir?
		return 4;
	}
	json_object_object_add(jsonRepository,
		"time", json_object_new_int64(getCurrentTime()));
	char* repositoryData = (char*)json_object_to_json_string_ext(
		jsonRepository, JSON_C_TO_STRING_PRETTY);
	if (repositoryData == NULL) {
		logPrintf(LOG_ERROR, "initRepo: json_object_to_json_string_ext() failed\n");
		json_object_put(jsonRepository);
		return 4;
	}

	// prepare encryption buffers
	size_t maxEncryptedBufSize = getMaxEncryptedBlockSize(
		strlen(repositoryData));
	char* encryptedBuf = malloc(maxEncryptedBufSize);
	if (encryptedBuf == NULL) {
		logPrintf(LOG_ERROR, "initRepo: malloc(): %s\n", strerror(errno));
		json_object_put(jsonRepository);
		return 5;
	}
	size_t encryptedBufSize = maxEncryptedBufSize;

	// encrypt
	int res = encryption->encrypt(repositoryData, strlen(repositoryData),
		encryptedBuf, &encryptedBufSize,
		passphrase);
	if (res != 0) {
		logPrintf(LOG_ERROR, "initRepo: encrypt failed: %d\n", res);
		json_object_put(jsonRepository);
		free(encryptedBuf);
		return 6;
	}

	// save
	res = destination->putRepositoryFile(encryptedBuf, encryptedBufSize);
	if (res != 0) {
		logPrintf(LOG_ERROR, "initRepo: destination->putRepositoryFile(): %d\n", res);
		json_object_put(jsonRepository);
		free(encryptedBuf);
		return 6;
	}

	json_object_put(jsonRepository);
	free(encryptedBuf);
	return 0;
}

int main(int argc, char *argv[])
{
	char *encryptionStr = NULL;
	char *name = NULL;
	char *comment = NULL;
	int ret = 0;

	static struct option longOptions[] = {
		{"passphrase-file", required_argument, NULL, 'P'},
		{NULL, 0, NULL, 0}
	};

	opterr = 0;
	
	confInit();

	int c;
	while ((c = getopt_long(argc, argv, ":Vhp:P:e:n:c:", longOptions, NULL)) != -1) {
		switch (c) {
			case 'V':
				fprintf(stdout, "bucse version %s\n", PACKAGE_VERSION);
				confCleanup();
				exit(0);
			case 'h':
				fprintf(stdout,
						"Initialize bucse repository\n"
						"\n"
						"Usage: bucse-init [options] <repository>\n"
						"\n"
						"Possible options:\n"
						"    -V                     print version\n"
						"    -h                     print help\n"
						"    -P FILE                read the passphrase from the first line of\n"
						"                           FILE ('-' for stdin); without -P or -p it's\n"
						"                           asked for on the terminal\n"
						"    --passphrase-file=FILE same as -P FILE\n"
						"    -p STRING              target repository passphrase; insecure:\n"
						"                           other local users can see it, and it stays\n"
						"                           in the shell history\n"
						"    -e STRING              encryption, can be 'none' or 'aes'\n"
						"    -n STRING              repository name (default: 'unnamed')\n"
						"    -c STRING              comment about repository\n"
				       );
				confCleanup();
				exit(0);
				break;
			case 'p':
				if (conf.passphrase != NULL) {
					wipeMemory(conf.passphrase, strlen(conf.passphrase));
					free(conf.passphrase);
				}
				conf.passphrase = strdup(optarg);
				// hide it from /proc/<pid>/cmdline and ps
				wipeMemory(optarg, strlen(optarg));
				if (conf.passphrase == NULL) {
					logPrintf(LOG_ERROR, "strdup(): %s\n", strerror(errno));
					ret = 1;
					goto out;
				}
				break;
			case 'P':
				free(conf.passphraseFile);
				conf.passphraseFile = strdup(optarg);
				if (conf.passphraseFile == NULL) {
					logPrintf(LOG_ERROR, "strdup(): %s\n", strerror(errno));
					ret = 1;
					goto out;
				}
				break;
			case 'e':
				encryptionStr = optarg;
				break;
			case 'n':
				name = optarg;
				break;
			case 'c':
				comment = optarg;
				break;
			case ':':
				if (optopt == 0)
					logPrintf(LOG_ERROR, "Option %s requires an argument.\n", argv[optind - 1]);
				else
					logPrintf(LOG_ERROR, "Option -%c requires an argument.\n", optopt);
				ret = 1;
				goto out;
			case '?':
				if (optopt == 0)
					logPrintf(LOG_ERROR, "Unknown option `%s'.\n", argv[optind - 1]);
				else if (isprint(optopt))
					logPrintf(LOG_ERROR, "Unknown option `-%c'.\n", optopt);
				else
					logPrintf(LOG_ERROR, "Unknown option character `\\x%x'.\n", optopt);
				ret = 1;
				goto out;
			default:
				abort ();
		}
	}

	if (conf.passphrase != NULL && conf.passphraseFile != NULL) {
		logPrintf(LOG_ERROR, "Give either -p or -P, not both.\n");
		ret = 1;
		goto out;
	}
	if (conf.passphrase != NULL) {
		logPrintf(LOG_WARNING, "a passphrase given on the command line can be seen "
			"by other local users, and stays in the shell history; leave it out to "
			"be asked for it, or use -P FILE\n");
	}

	if (optind >= argc) {
		logPrintf(LOG_ERROR, "No repository given, see bucse-init -h\n");
		ret = 1;
		goto out;
	}

	encryption = getEncryptionByName(encryptionStr);
	if (encryption == NULL ) {
		ret = 2;
		goto out;
	}
	if (encryption->needsPassphrase() && conf.passphrase == NULL) {
		if (conf.passphraseFile != NULL) {
			conf.passphrase = readPassphraseFile(conf.passphraseFile);
		} else if (passphraseTerminalAvailable()) {
			// a typo here would make the repository unreadable
			conf.passphrase = readPassphraseTerminal(1);
		}
		if (conf.passphrase == NULL) {
			logPrintf(LOG_ERROR, "Password is necessary for %s encryption\n",
				encryptionStr);
			ret = 3;
			goto out;
		}
	}

	int index;
	for (index = optind; index < argc; index++) {
		// not a sum of the results: it could wrap around to 0 as an exit code
		if (initRepo(argv[index], conf.passphrase, encryptionStr,
			name, comment) != 0)
			ret = 4;
	}

out:
	confCleanup();
	return ret;
}

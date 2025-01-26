/*
 * bucse-init.c
 *
 * The program for creating and initializing a bucse repository.
 */

#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#include <json.h>

#include "conf.h"
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
	free(realPath);

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

	destination->putRepositoryJsonFile(jsonData, strlen(jsonData));

	json_object_put(jsonRepositoryJson);

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
	char *passphrase = NULL;
	char *encryptionStr = NULL;
	char *name = NULL;
	char *comment = NULL;

	opterr = 0;
	
	confInit();

	int c;
	while ((c = getopt (argc, argv, "Vhp:e:n:c:")) != -1) {
		switch (c) {
			case 'V':
				// TODO: version
				break;
			case 'h':
				// TODO: help
				break;
			case 'p':
				passphrase = optarg;
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
			case '?':
				if (optopt == 'p')
					logPrintf(LOG_ERROR, "Option -%c requires an argument.\n", optopt);
				else if (isprint(optopt))
					logPrintf(LOG_ERROR, "Unknown option `-%c'.\n", optopt);
				else
					logPrintf(LOG_ERROR, "Unknown option character `\\x%x'.\n", optopt);
				return 1;
			default:
				abort ();
		}
	}

	encryption = getEncryptionByName(encryptionStr);
	if (encryption == NULL ) {
		return 2;
	}
	if (encryption->needsPassphrase() && passphrase == NULL) {
		// TODO: password prompt
		logPrintf(LOG_ERROR, "Password is necessary for %s encryption\n",
			encryptionStr);
		return 3;
	}

	int index;
	int ret = 0;
	for (index = optind; index < argc; index++)
		ret += initRepo(argv[index], passphrase, encryptionStr,
			name, comment);

	return ret;
}


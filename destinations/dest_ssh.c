#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include <json.h>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include "dest.h"

static char* repositoryHost;
static char* repositoryPort;
static char* repositoryPath;

static char* repositoryJsonFilePath;
static char* repositoryFilePath;
static char* repositoryActionsPath;
static char* repositoryStoragePath;

static ActionAddedCallback cachedActionAddedCallback;

// TODO: rename to ActionNames [?]
typedef struct {
	char* names;
	int len;
	int size;
} Actions;

static int addAction(Actions *actions, char* newActionName)
{
	int oldActionsSize = actions->size;

	if (actions->len == actions->size) {
		int newActionSize;

		if (actions->size == 0) {
			newActionSize = 16;
		} else {
			newActionSize = oldActionsSize * 2;
		}

		char* newNames = malloc(newActionSize * MAX_ACTION_NAME_LEN);
		if (newNames == NULL) {
			fprintf(stderr, "addAction: malloc(): %s\n", strerror(errno));
			return 1;
		}
		if (actions->names != NULL) {
			memcpy(newNames, actions->names, oldActionsSize * MAX_ACTION_NAME_LEN);
			free(actions->names);
		}
		actions->names = newNames;
		actions->size = newActionSize;
	}

	snprintf(actions->names + (MAX_ACTION_NAME_LEN * actions->len), MAX_ACTION_NAME_LEN, "%s", newActionName);
	actions->len++;

	return 0;
}

static void freeActions(Actions *actions)
{
	if (actions->names != NULL) {
		free(actions->names);
	}
	actions->names = NULL;
	actions->len = actions->size = 0;
}

static char* getAction(Actions *actions, int index)
{
	return actions->names + (MAX_ACTION_NAME_LEN * index);
}

static int findAction(Actions *actions, char* actionNameToFound)
{
	for (int i=0; i<actions->len; i++) {
		if (strncmp(actions->names + (MAX_ACTION_NAME_LEN * i), actionNameToFound, MAX_ACTION_NAME_LEN) == 0) {
			return i;
		}
	}
	return -1;
}

static Actions handledActions;

/*
 * copy-paste from https://api.libssh.org/stable/libssh_tutor_guided_tour.html
 */
static int verify_knownhost(ssh_session session)
{
	enum ssh_known_hosts_e state;
	unsigned char *hash = NULL;
	ssh_key srv_pubkey = NULL;
	size_t hlen;
	char buf[10];
	char *hexa;
	char *p;
	int cmp;
	int rc;

	rc = ssh_get_server_publickey(session, &srv_pubkey);
	if (rc < 0) {
		return -1;
	}

	rc = ssh_get_publickey_hash(srv_pubkey,
			SSH_PUBLICKEY_HASH_SHA1,
			&hash,
			&hlen);
	ssh_key_free(srv_pubkey);
	if (rc < 0) {
		return -1;
	}

	state = ssh_session_is_known_server(session);
	switch (state) {
		case SSH_KNOWN_HOSTS_OK:
			/* OK */

			break;
		case SSH_KNOWN_HOSTS_CHANGED:
			fprintf(stderr, "Host key for server changed.\n");
			//fprintf(stderr, "Host key for server changed: it is now:\n");
			//ssh_print_hexa("Public key hash", hash, hlen);
			fprintf(stderr, "For security reasons, connection will be stopped\n");
			ssh_clean_pubkey_hash(&hash);

			return -1;
		case SSH_KNOWN_HOSTS_OTHER:
			fprintf(stderr, "The host key for this server was not found but an other"
					"type of key exists.\n");
			fprintf(stderr, "An attacker might change the default server key to"
					"confuse your client into thinking the key does not exist\n");
			ssh_clean_pubkey_hash(&hash);

			return -1;
		case SSH_KNOWN_HOSTS_NOT_FOUND:
			fprintf(stderr, "Could not find known host file.\n");
			fprintf(stderr, "If you accept the host key here, the file will be"
					"automatically created.\n");

			/* FALL THROUGH to SSH_SERVER_NOT_KNOWN behavior */

		case SSH_KNOWN_HOSTS_UNKNOWN:
			hexa = ssh_get_hexa(hash, hlen);
			fprintf(stderr,"The server is unknown. Do you trust the host key?\n");
			fprintf(stderr, "Public key hash: %s\n", hexa);
			ssh_string_free_char(hexa);
			ssh_clean_pubkey_hash(&hash);
			p = fgets(buf, sizeof(buf), stdin);
			if (p == NULL) {
				return -1;
			}

			cmp = strncasecmp(buf, "yes", 3);
			if (cmp != 0) {
				return -1;
			}

			rc = ssh_session_update_known_hosts(session);
			if (rc < 0) {
				fprintf(stderr, "Error %s\n", strerror(errno));
				return -1;
			}

			break;
		case SSH_KNOWN_HOSTS_ERROR:
			fprintf(stderr, "Error %s", ssh_get_error(session));
			ssh_clean_pubkey_hash(&hash);
			return -1;
	}

	ssh_clean_pubkey_hash(&hash);
	return 0;
}

static ssh_session bucseSshSession;
static sftp_session bucseSftpSession;
static void invalidDestination() {
	fprintf(stderr, "Invalid destination. Expected format ssh://[host]{:[port]}/[path]\n");
}
static void cleanupString(char **s) {
	if (*s != NULL) {
		free(*s);
		*s = NULL;
	}
}

static void cleanupStrings() {
	cleanupString(&repositoryHost);
	cleanupString(&repositoryPort);
	cleanupString(&repositoryPath);
	cleanupString(&repositoryJsonFilePath);
	cleanupString(&repositoryFilePath);
	cleanupString(&repositoryActionsPath);
	cleanupString(&repositoryStoragePath);
}

int destSshInit(char* repository)
{
	char* firstSlash = strstr(repository, "/");
	char* firstColon = strstr(repository, ":");
	int port = 22;

	if (firstSlash == NULL) {
		invalidDestination();
		return 1;
	}
	if (firstColon && firstColon < firstSlash) {
		repositoryPort = malloc(firstSlash - firstColon);
		if (repositoryPort == NULL) {
			invalidDestination();
			cleanupStrings();
			return 2;
		}
		memcpy(repositoryPort, firstColon+1, firstSlash - firstColon - 1);
		repositoryPort[firstSlash - firstColon - 1] = 0;

		repositoryHost = malloc(firstColon - repository + 1);
		if (repositoryHost == NULL) {
			invalidDestination();
			cleanupStrings();
			return 3;
		}
		memcpy(repositoryHost, repository, firstColon - repository);
		repositoryHost[firstColon - repository] = 0;
	} else {
		repositoryHost = malloc(firstSlash - repository + 1);
		if (repositoryHost == NULL) {
			invalidDestination();
			cleanupStrings();
			return 4;
		}
		memcpy(repositoryHost, repository, firstSlash - repository);
		repositoryHost[firstSlash - repository] = 0;
	}

	// handle path relative to home
	if (strncmp(firstSlash, "/~/", 3) == 0) {
		firstSlash += 3;
	}

	repositoryPath = malloc(strlen(firstSlash)+1);
	if (repositoryPath == NULL) {
		invalidDestination();
		cleanupStrings();
		return 5;
	}
	memcpy(repositoryPath, firstSlash, strlen(firstSlash));
	repositoryPath[strlen(firstSlash)] = 0;

	if (sscanf(repositoryPort, "%d", &port) <= 0) {
		invalidDestination();
		cleanupStrings();
		return 6;
	}

	if (port <= 0 || port > 0xffff) {
		invalidDestination();
		cleanupStrings();
		return 7;
	}

	// construct file path of repository.json file
	repositoryJsonFilePath = malloc(MAX_FILEPATH_LEN);
	if (repositoryJsonFilePath == NULL) {
		fprintf(stderr, "destSshInit: malloc(): %s\n", strerror(errno));

		cleanupStrings();
		return 1;
	}
	repositoryFilePath = malloc(MAX_FILEPATH_LEN);
	if (repositoryFilePath == NULL) {
		fprintf(stderr, "destSshInit: malloc(): %s\n", strerror(errno));

		cleanupStrings();
		return 2;
	}
	repositoryActionsPath = malloc(MAX_FILEPATH_LEN);
	if (repositoryActionsPath == NULL) {
		fprintf(stderr, "destSshInit: malloc(): %s\n", strerror(errno));

		cleanupStrings();
		return 3;
	}
	repositoryStoragePath = malloc(MAX_FILEPATH_LEN);
	if (repositoryStoragePath == NULL) {
		fprintf(stderr, "destSshInit: malloc(): %s\n", strerror(errno));

		cleanupStrings();
		return 4;
	}

	snprintf(repositoryJsonFilePath, MAX_FILEPATH_LEN, "%s/repository.json", repositoryPath);
	snprintf(repositoryFilePath, MAX_FILEPATH_LEN, "%s/repository", repositoryPath);
	snprintf(repositoryActionsPath, MAX_FILEPATH_LEN, "%s/actions", repositoryPath);
	snprintf(repositoryStoragePath, MAX_FILEPATH_LEN, "%s/storage", repositoryPath);

	bucseSshSession = ssh_new();
	if (bucseSshSession == NULL) {
		cleanupStrings();
		return 1;
	}
	ssh_options_set(bucseSshSession, SSH_OPTIONS_HOST, repositoryHost);
	ssh_options_set(bucseSshSession, SSH_OPTIONS_PORT, &port);

	// Connect to server
	int rc = ssh_connect(bucseSshSession);
	if (rc != SSH_OK)
	{
		fprintf(stderr, "Error connecting to localhost: %s\n",
			ssh_get_error(bucseSshSession));
		ssh_free(bucseSshSession);
		bucseSshSession = NULL;
		cleanupStrings();
		return 2;
	}

	// Verify the server's identity
	// For the source code of verify_knownhost(), check previous example
	if (verify_knownhost(bucseSshSession) < 0)
	{
		ssh_disconnect(bucseSshSession);
		ssh_free(bucseSshSession);
		bucseSshSession = NULL;
		cleanupStrings();
		return 3;
	}

	// Authenticate ourselves
	rc = ssh_userauth_publickey_auto(bucseSshSession, NULL, NULL);

	if (rc == SSH_AUTH_ERROR)
	{
		fprintf(stderr, "Authentication failed: %s\n",
			ssh_get_error(bucseSshSession));
		ssh_disconnect(bucseSshSession);
		ssh_free(bucseSshSession);
		bucseSshSession = NULL;
		cleanupStrings();
		return 4;
	}

	// sftp
	bucseSftpSession = sftp_new(bucseSshSession);
	if (bucseSftpSession == NULL)
	{
		fprintf(stderr, "Error allocating SFTP session: %s\n",
			ssh_get_error(bucseSshSession));
		ssh_disconnect(bucseSshSession);
		ssh_free(bucseSshSession);
		bucseSshSession = NULL;
		cleanupStrings();
		return 5;
	}

	rc = sftp_init(bucseSftpSession);
	if (rc != SSH_OK)
	{
		fprintf(stderr, "Error initializing SFTP session: code %d.\n",
			sftp_get_error(bucseSftpSession));
		sftp_free(bucseSftpSession);
		bucseSftpSession = NULL;
		ssh_disconnect(bucseSshSession);
		ssh_free(bucseSshSession);
		bucseSshSession = NULL;
		cleanupStrings();
		return 6;
	}

	return 0;
}

void destSshShutdown()
{
	cleanupStrings();

	freeActions(&handledActions);

	if (bucseSftpSession != NULL) {
		sftp_free(bucseSftpSession);
		bucseSftpSession = NULL;
	}
	if (bucseSshSession != NULL) {
		ssh_disconnect(bucseSshSession);
		ssh_free(bucseSshSession);	
		bucseSshSession = NULL;
	}
}

int destSshPutStorageFile(const char* filename, char *buf, size_t size)
{
	char* storageFilePath = malloc(MAX_FILEPATH_LEN);
	if (storageFilePath == NULL) {
		fprintf(stderr, "destSshPutStorageFile: malloc(): %s\n", strerror(errno));

		return 1;
	}

	snprintf(storageFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryStoragePath, filename);

	sftp_file file = sftp_open(bucseSftpSession, storageFilePath, O_WRONLY | O_CREAT | O_EXCL, 0644);
	free(storageFilePath);
	if (file == NULL) {
		fprintf(stderr, "destSshPutStorageFile: sftp_open(): %s\n",
			sftp_get_error(bucseSftpSession));
		return 2;
	}

	int bytesWritten = sftp_write(file, buf, size);
	if (bytesWritten < 0) {
		fprintf(stderr, "destSshPutStorageFile: sftp_write(): %s\n",
			sftp_get_error(bucseSftpSession));
		sftp_close(file);
		return 3;
	}
	sftp_close(file);

	return 0;
}

int destSshGetStorageFile(const char* filename, char *buf, size_t *size)
{
	char* storageFilePath = malloc(MAX_FILEPATH_LEN);
	if (storageFilePath == NULL) {
		fprintf(stderr, "destSshGetStorageFile: malloc(): %s\n", strerror(errno));

		return 1;
	}

	snprintf(storageFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryStoragePath, filename);

	sftp_file file = sftp_open(bucseSftpSession, storageFilePath, O_RDONLY, 0);
	free(storageFilePath);

	if (file == NULL) {
		fprintf(stderr, "destSshInit: sftp_open(): %d\n",
			sftp_get_error(bucseSftpSession));

		return 2;
	}

	int bytesRead = sftp_read(file, buf, *size);
	sftp_close(file);

	if (bytesRead >= *size) {
		fprintf(stderr, "destSshInit: repository.json file is too large\n");

		return 2;
	}

	buf[bytesRead] = 0; // null termination
	*size = bytesRead;
	return 0;
}

int destSshAddActionFile(char* filename, char *buf, size_t size)
{
	char* actionFilePath = malloc(MAX_FILEPATH_LEN);
	if (actionFilePath == NULL) {
		fprintf(stderr, "destSshAddActionFile: malloc(): %s\n", strerror(errno));

		return 1;
	}

	snprintf(actionFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryActionsPath, filename);

	sftp_file file = sftp_open(bucseSftpSession, actionFilePath, O_WRONLY | O_CREAT | O_EXCL, 0644);
	free(actionFilePath);
	if (file == NULL) {
		fprintf(stderr, "destSshAddActionFile: sftp_open(): %s\n",
			sftp_get_error(bucseSftpSession));
		return 2;
	}

	int bytesWritten = sftp_write(file, buf, size);
	if (bytesWritten < 0) {
		fprintf(stderr, "destSshAddActionFile: sftp_write(): %s\n",
			sftp_get_error(bucseSftpSession));
		sftp_close(file);
		return 3;
	}
	sftp_close(file);

	addAction(&handledActions, filename);
	return 0;
}

int destSshGetRepositoryJsonFile(char *buf, size_t *size)
{
	sftp_file file = sftp_open(bucseSftpSession, repositoryJsonFilePath, O_RDONLY, 0);
	if (file == NULL) {
		fprintf(stderr, "destSshInit: sftp_open(): %d\n",
			sftp_get_error(bucseSftpSession));

		return 1;
	}

	int bytesRead = sftp_read(file, buf, *size);
	sftp_close(file);

	if (bytesRead >= *size) {
		fprintf(stderr, "destSshGetRepositoryJsonFile: repository.json file is too large\n");

		return 2;
	}

	buf[bytesRead] = 0; // null termination
	*size = bytesRead;
	return 0;
}

int destSshGetRepositoryFile(char *buf, size_t *size)
{
	sftp_file file = sftp_open(bucseSftpSession, repositoryFilePath, O_RDONLY, 0);
	if (file == NULL) {
		fprintf(stderr, "destSshInit: sftp_open(): %d\n",
			sftp_get_error(bucseSftpSession));

		return 1;
	}

	int bytesRead = sftp_read(file, buf, *size);
	sftp_close(file);

	if (bytesRead >= *size) {
		fprintf(stderr, "destSshGetRepositoryJsonFile: repository.json file is too large\n");

		return 2;
	}

	buf[bytesRead] = 0; // null termination
	*size = bytesRead;
	return 0;
}

int destSshSetCallbackActionAdded(ActionAddedCallback callback)
{
	cachedActionAddedCallback = callback;
	return 0;
}

int destSshIsTickable()
{
	return 1;
}

int destSshTick()
{
#define TICK_PERIOD_SECONDS 10

	static int counter = 0;
	if (--counter > 0) {
		return 0;
	}
	counter = TICK_PERIOD_SECONDS;

	sftp_dir actionsDir = sftp_opendir(bucseSftpSession, repositoryActionsPath);
	if (actionsDir == NULL) {
		fprintf(stderr, "warning: destSshTick(): sftp_opendir(): %s\n",
			ssh_get_error(bucseSshSession));
		return 0;
	}

	Actions newActions;
	memset(&newActions, 0, sizeof(Actions));

	for (;;) {
		errno = 0;
		sftp_attributes actionDir = sftp_readdir(bucseSftpSession, actionsDir);
		if (actionDir == NULL) {
			break;
		}
		if (actionDir->name && actionDir->name[0] == '.') {
			sftp_attributes_free(actionDir);
			continue;
		}

		// TODO: consider optimizing by keeping handledActions sorted and searching with binary search
		
		// is the action not already handled?
		if (findAction(&handledActions, actionDir->name) == -1) {
			addAction(&newActions, actionDir->name);
		}

		sftp_attributes_free(actionDir);
	}
	sftp_closedir(actionsDir);

	printf("DEBUG: new actions count: %d\n", newActions.len);

	char* actionFilePath = malloc(MAX_FILEPATH_LEN);
	if (actionFilePath == NULL) {
		fprintf(stderr, "destSshTick: malloc(): %s\n", strerror(errno));

		return 1;
	}
	char* actionFileBuf = malloc(MAX_ACTION_LEN);
	if (actionFileBuf == NULL) {
		fprintf(stderr, "destSshTick: malloc(): %s\n", strerror(errno));

		free(actionFilePath);
		return 1;
	}

	for (int i=0; i<newActions.len; i++) {
		printf("DEBUG: handle new action: %s\n", getAction(&newActions, i));

		snprintf(actionFilePath, MAX_FILEPATH_LEN, "%s/%s", repositoryActionsPath, getAction(&newActions, i));

		sftp_file file = sftp_open(bucseSftpSession, actionFilePath, O_RDONLY, 0);
		if (file == NULL) {
			fprintf(stderr, "destSshTick: sftp_open(): %s\n",
				ssh_get_error(bucseSshSession));

			continue;
		}

		int bytesRead = sftp_read(file, actionFileBuf, MAX_ACTION_LEN);
		sftp_close(file);

		if (bytesRead >= MAX_ACTION_LEN) {
			fprintf(stderr, "destSshTick: action file is too large\n");
			continue;
		}

		actionFileBuf[bytesRead] = 0; // null termination
		if (cachedActionAddedCallback) {
			cachedActionAddedCallback(getAction(&newActions, i), actionFileBuf, bytesRead, newActions.len - i - 1);
		} else {
			fprintf(stderr, "destSshTick: no action added callback\n");
		}
	}
	free(actionFilePath);
	free(actionFileBuf);
	
	for (int i=0; i<newActions.len; i++) {
		addAction(&handledActions, getAction(&newActions, i));
	}


	freeActions(&newActions);

	return 0;
}

Destination destinationSsh = {
	.init = destSshInit,
	.shutdown = destSshShutdown,
	.putStorageFile = destSshPutStorageFile,
	.getStorageFile = destSshGetStorageFile,
	.addActionFile = destSshAddActionFile,
	.getRepositoryJsonFile = destSshGetRepositoryJsonFile,
	.getRepositoryFile = destSshGetRepositoryFile,
	.setCallbackActionAdded = destSshSetCallbackActionAdded,
	.isTickable = destSshIsTickable,
	.tick = destSshTick,
};


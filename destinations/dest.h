/*
 * destinations/dest.h
 *
 * Destination is a polymorphic representation of where the repository is.
 * A destination is identified by the 'repository' string on init(). Also, the
 * prefix of this string denotes a type (implementation) of the destination.
 *
 * Implementations:
 * - A directory elswhere in a filesystem.
 *   implemented in: destinations/dest_local.c,
 *   repository prefix: file://
 * - A directory accessed via ssh (sftp).
 *   implemented in: destinations/dest_ssh.c,
 *   repository prefix: ssh://
 */

#define MAX_FILEPATH_LEN 1024

#define MAX_ACTION_LEN (1024 * 1024)
#define MAX_ACTION_NAME_LEN 64

typedef void (*ActionAddedCallback)(char* actionName, char* buf, size_t size, int moreInThisBatch);

typedef struct {
	int (*init)(char* repository);
	void (*shutdown)();
	int (*putStorageFile)(const char* filename, char *buf, size_t size);
	int (*getStorageFile)(const char* filename, char *buf, size_t *size);
	int (*addActionFile)(char* filename, char *buf, size_t size);
	int (*getRepositoryJsonFile)(char *buf, size_t *size);
	int (*getRepositoryFile)(char *buf, size_t *size);
	int (*setCallbackActionAdded)(ActionAddedCallback callback);
	int (*isTickable)();
	int (*tick)();
} Destination;

// auxiliary function that returns 20 random bytes as a hex string. filename
// needs to point to a buffer of size at least 41 bytes, prefferably
// MAX_ACTION_NAME_LEN bytes.
int getRandomStorageFileName(char* filename);

void getDestinationBasedOnPathPrefix(Destination** destPtr,
	char** realPathPtr,
	char* path);

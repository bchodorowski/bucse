#define PACKAGE_VERSION "0.1.1-current"

struct bucse_config {
	char *repository;
	int verbose;
	char *passphrase;
	char *passphraseFile;
	char *repositoryRealPath;
	int readOnly;
	int checkInterval;
};

extern struct bucse_config conf;

#include <stddef.h>

void confInit();
void confCleanup();

int confIsReadOnly();

void wipeMemory(void *p, size_t n);

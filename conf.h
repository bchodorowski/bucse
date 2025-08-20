#define PACKAGE_VERSION "0.1.0-current"

struct bucse_config {
	char *repository;
	int verbose;
	char *passphrase;
	char *repositoryRealPath;
};

extern struct bucse_config conf;

void confInit();
void confCleanup();

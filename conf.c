#include <stdlib.h>

#include "conf.h"

struct bucse_config conf;

void confCleanup()
{
	if (conf.repository) {
		free(conf.repository);
		conf.repository = NULL;
	}
	if (conf.repositoryRealPath) {
		free(conf.repositoryRealPath);
		conf.repositoryRealPath = NULL;
	}
	if (conf.passphrase) {
		free(conf.passphrase);
		conf.passphrase = NULL;
	}
}


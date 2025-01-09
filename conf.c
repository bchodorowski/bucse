#include <stdlib.h>
#include <string.h>

#include "conf.h"

struct bucse_config conf;

void confInit()
{
	memset(&conf, 0, sizeof(conf));
	conf.verbose = 2;
}

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


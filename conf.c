#include <stdlib.h>
#include <string.h>

#include "conf.h"

struct bucse_config conf;

void confInit()
{
	memset(&conf, 0, sizeof(conf));
	conf.verbose = 2;
	conf.readOnly = 0;
	conf.checkInterval = 10;
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
		wipeMemory(conf.passphrase, strlen(conf.passphrase));
		free(conf.passphrase);
		conf.passphrase = NULL;
	}
	if (conf.passphraseFile) {
		free(conf.passphraseFile);
		conf.passphraseFile = NULL;
	}
}

int confIsReadOnly()
{
	// TODO: is read only or snapshot from given datetime requested
	return conf.readOnly;
}

// Overwrites a secret with zeros. A memset() of a buffer that is never read
// again can be optimised away; writes through a volatile pointer can't.
// (explicit_bzero() would do the same, but it isn't C99.)
void wipeMemory(void *p, size_t n)
{
	volatile unsigned char *v = p;
	while (n > 0) {
		*v++ = 0;
		n--;
	}
}

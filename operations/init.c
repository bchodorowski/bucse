#include <fuse.h>
#include <pthread.h>

#include "../actions.h"

#include "operations.h"

#include "init.h"

static void* bucse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	//cfg->use_ino = 1;
	cfg->hard_remove = 1;

	return NULL;
}

void* bucse_init_guarded(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	pthread_mutex_lock(&bucseMutex);
	void* result = bucse_init(conn, cfg);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}


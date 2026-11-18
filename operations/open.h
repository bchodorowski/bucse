// bucse_open() expects bucseMutex to be held, bucse_open_guarded() locks it
int bucse_open(const char *path, struct fuse_file_info *fi);
int bucse_open_guarded(const char *path, struct fuse_file_info *fi);


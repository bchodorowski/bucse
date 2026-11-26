#include <stdint.h>

int bucse_mkdir(const char *path, mode_t mode);
int bucse_mkdir_guarded(const char *path, mode_t mode);

// bucse_mkdir(), and if hasMtime, the directory gets mtime (in nanoseconds)
// instead of the current time (a renamed directory keeps its mtime)
int makeDirectory(const char *path, int hasMtime, int64_t mtime);

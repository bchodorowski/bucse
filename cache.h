/*
 * Prepares the cache mechanism.
 *
 * @return 0 on success, error code on error
 */
int cacheInit();

/*
 * Gets value from cache.
 *
 * @param block A block to get.
 * @param buf Buffer where data should be written to.
 * @param size Pointer to a variable that stores size of the buffer. In cache entry is found, the value will be set to the block size.
 * @return 0 when cache found, -1 when not found, positive error code on error
 */
int cacheGet(const char* block, char* buf, size_t *size);

/*
 * Puts value to cache.
 *
 * @param block A block to put.
 * @param buf Buffer that holds the data.
 * @param size Block size.
 * @return 0 on success, error code on error
 */
int cachePut(const char* block, char* buf, size_t size);

/*
 * Cleans up the cache mechanism.
 */
void cacheCleanup();

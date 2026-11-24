/*
 * Prepares the cache mechanism.
 *
 * @return 0 on success, error code on error
 */
int cacheInit();

/*
 * Gets value from cache, without copying it.
 *
 * @param block A block to get.
 * @param data Set to the cached data when the block is found. It is owned by
 *             the cache and stays valid only until the next cachePut() or
 *             cacheCleanup() call: copy what is needed right away.
 * @param size Set to the block size when the block is found.
 * @return 0 when cache found, -1 when not found
 */
int cacheGet(const char* block, const char** data, size_t *size);

/*
 * Puts value to cache. The cache takes ownership of data (a malloc()ed
 * buffer) on success, and frees it when the entry is evicted. On error the
 * caller still owns data. The new entry itself is never evicted by this call,
 * even if it is larger than the cache.
 *
 * @param block A block to put. The string must stay valid while the block is
 *              cached.
 * @param data malloc()ed buffer that holds the data.
 * @param size Block size.
 * @return 0 on success, error code on error
 */
int cachePut(const char* block, char* data, size_t size);

/*
 * Cleans up the cache mechanism.
 */
void cacheCleanup();

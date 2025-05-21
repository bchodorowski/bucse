#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "log.h"
#include "dynarray.h"

#include "cache.h"

typedef struct _BlockslistItem {
	struct _BlockslistItem *prev;
	struct _BlockslistItem *next;
	const char* key;
} BlockslistItem;

typedef struct {
	BlockslistItem *first;
	BlockslistItem *last;
} Blockslist;

typedef struct {
	const char* key;
	size_t size;
	char* data;
	BlockslistItem *blockslistItem;
} Block;

#define HASH_TABLE_BUCKETS_COUNT_AS_HEX_CHARS 3
#define HASH_TABLE_BUCKETS_COUNT (1 << (4*HASH_TABLE_BUCKETS_COUNT_AS_HEX_CHARS))

#define HASH_TABLE_SIZE_COUNT 1024
#define HASH_TABLE_SIZE_BYTES 250*1024*1024

static DynArray hashTableBuckets[HASH_TABLE_BUCKETS_COUNT];

static Blockslist blockslist;
int blockslistCount;
int blockslistBytes;

static int hexStringToHashIndex(const char* hex)
{
	int stringLen = strlen(hex);
	int result = 0;
	for (int i=0; i<HASH_TABLE_BUCKETS_COUNT_AS_HEX_CHARS; i++) {
		if (i >= stringLen) {
			continue;
		}

		int charValue = 0;
		if (hex[i] >= '0' && hex[i] <= '9') {
			charValue = (int)(hex[i] - '0');
		} else if (hex[i] >= 'a' && hex[i] <= 'f') {
			charValue = (int)(hex[i] - 'a') + 10;
		} else if (hex[i] >= 'A' && hex[i] <= 'F') {
			charValue = (int)(hex[i] - 'A') + 10;
		}

		result += charValue * (1 << (4*(HASH_TABLE_BUCKETS_COUNT_AS_HEX_CHARS-1-i)));
	}
	return result;
}

int cacheInit()
{
	// should be zeroed at start, but let's clean everything anyway
	memset(&hashTableBuckets, 0, sizeof(DynArray)*HASH_TABLE_BUCKETS_COUNT);
	memset(&blockslist, 0, sizeof(Blockslist));
	blockslistCount = 0;
	blockslistBytes = 0;

	return 0;
}

int cacheGet(const char* block, char* buf, size_t *size)
{
	int index = hexStringToHashIndex(block);

	for (int i=0; i<hashTableBuckets[index].len; i++) {
		Block* item = (Block*)hashTableBuckets[index].objects[i];

		if (strcmp(item->key, block) != 0) {
			continue;
		}

		// item is found in the hashtable

		memcpy(buf, item->data, item->size);
		*size = item->size;

		// move item in the blockslist to the front
		if (item->blockslistItem != blockslist.first) {
			if (item->blockslistItem == blockslist.last)
				blockslist.last = item->blockslistItem->prev;
			if (item->blockslistItem->prev)
				item->blockslistItem->prev->next = item->blockslistItem->next;
			if (item->blockslistItem->next)
				item->blockslistItem->next->prev = item->blockslistItem->prev;
			if (blockslist.first)
				blockslist.first->prev = item->blockslistItem;
			item->blockslistItem->next = blockslist.first;
			item->blockslistItem->prev = NULL;
			blockslist.first = item->blockslistItem;

		}

		logPrintf(LOG_VERBOSE_DEBUG, "cacheGet: cache hit: %s\n", block);
		return 0;
	}
	logPrintf(LOG_VERBOSE_DEBUG, "cacheGet: cache miss: %s\n", block);
	return -1;
}

int cachePut(const char* block, char* buf, size_t size)
{
	int index = hexStringToHashIndex(block);
	Block* newItem = (Block*)malloc(sizeof(Block));
	if (!newItem) {
		logPrintf(LOG_ERROR, "cachePut: malloc(): %s\n", strerror(errno));
		return 1;
	}

	BlockslistItem* newBlockslistItem = (BlockslistItem*)malloc(sizeof(BlockslistItem));
	if (!newBlockslistItem) {
		logPrintf(LOG_ERROR, "cachePut: malloc(): %s\n", strerror(errno));
		free(newItem);
		return 2;
	}

	newItem->key = block;
	newItem->size = size;
	newItem->data = malloc(size);
	if (!newItem->data) {
		logPrintf(LOG_ERROR, "cachePut: malloc(): %s\n", strerror(errno));
		free(newItem);
		free(newBlockslistItem);
		return 3;
	}
	memcpy(newItem->data, buf, size);
	addToDynArray(&hashTableBuckets[index], newItem);

	// add item to the front blockslist
	newBlockslistItem->prev = NULL;
	newBlockslistItem->next = blockslist.first;
	newBlockslistItem->key = block;
	blockslist.first = newBlockslistItem;
	if (blockslist.first->next) {
		blockslist.first->next->prev = blockslist.first;
	}
	if (blockslist.last == NULL)
		blockslist.last = blockslist.first;

	newItem->blockslistItem = newBlockslistItem;
	
	blockslistCount ++;
	blockslistBytes += size;

	logPrintf(LOG_VERBOSE_DEBUG, "cachePut: cache item saved: %s\n", block);

	// check blockslist count and bytes size and delete oldest entries if needed
	while (blockslistCount > HASH_TABLE_SIZE_COUNT || blockslistBytes > HASH_TABLE_SIZE_BYTES) {
		int indexToDelete = hexStringToHashIndex(blockslist.last->key);
		Block* foundItemToDelete = NULL;
		int foundItemToDeleteIndex = -1;
		for (int i=0; i<hashTableBuckets[indexToDelete].len; i++) {
			Block* itemToDelete = (Block*)hashTableBuckets[indexToDelete].objects[i];
			if (strcmp(itemToDelete->key, blockslist.last->key) == 0) {
				foundItemToDelete = itemToDelete;
				foundItemToDeleteIndex = i;
				break;
			}
		}
		if (!foundItemToDelete) {
			logPrintf(LOG_WARNING, "cachePut: block expected but not found in cache\n");
			return 0;
		}
		blockslistCount --;
		blockslistBytes -= foundItemToDelete->size;
		free(foundItemToDelete->data);
		removeFromDynArrayUnorderedByIndex(&hashTableBuckets[indexToDelete], foundItemToDeleteIndex);
		free(foundItemToDelete);

		BlockslistItem* toDelete = blockslist.last;
		blockslist.last = blockslist.last->prev;
		if (toDelete)
		{
			logPrintf(LOG_VERBOSE_DEBUG, "cachePut: cache item removed: %s\n", toDelete->key);
			free(toDelete);
		}
		if (blockslist.last)
			blockslist.last->next = NULL;
	}

	return 0;
}

void cacheCleanup()
{
	// free and clear the hash table
	for (int index = 0; index < HASH_TABLE_BUCKETS_COUNT; index++) {
		for (int i=0; i<hashTableBuckets[index].len; i++) {
			Block* item = (Block*)hashTableBuckets[index].objects[i];
			free(item->data);
			free(item);
		}
		freeDynArray(&hashTableBuckets[index]);
	}

	// free and clear the list
	while (blockslist.first) {
		BlockslistItem* toFree = blockslist.first;
		blockslist.first = blockslist.first->next;
		free(toFree);
	}

	// zero everything just because why not
	memset(&hashTableBuckets, 0, sizeof(DynArray)*HASH_TABLE_BUCKETS_COUNT);
	memset(&blockslist, 0, sizeof(Blockslist));
	blockslistCount = 0;
	blockslistBytes = 0;
}


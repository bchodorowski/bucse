#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "log.h"
#include "dynarray.h"

#include "cache.h"

struct Block;

typedef struct _BlockslistItem {
	struct _BlockslistItem *prev;
	struct _BlockslistItem *next;
	struct Block *data;
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
#define HASH_TABLE_SIZE_BYTES 100*1024*1024

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
		printf("%p\n", item);

		if (strcmp(item->key, block) != 0) {
			continue;
		}

		// item is found in the hashtable

		memcpy(buf, item->data, item->size);
		*size = item->size;

		// move item in the blockslist to the front
		if (item->blockslistItem->prev)
			item->blockslistItem->prev->next = item->blockslistItem->next;
		if (item->blockslistItem->next)
			item->blockslistItem->next->prev = item->blockslistItem->prev;
		if (blockslist.first)
			blockslist.first->prev = item->blockslistItem;
		item->blockslistItem->next = blockslist.first;
		item->blockslistItem->prev = NULL;
		blockslist.first = item->blockslistItem;

		return 0;
	}
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


	newItem->key = block;
	newItem->size = size;
	newItem->data = malloc(size);
	if (!newItem->data) {
		logPrintf(LOG_ERROR, "cachePut: malloc(): %s\n", strerror(errno));
		free(newItem);
		return 2;
	}
	memcpy(newItem->data, buf, size);
	addToDynArray(&hashTableBuckets[index], newItem);

	// TODO: add item to the front blockslist
	
	// TODO: check blockslist count and bytes size and delete oldest entries if needed

	return 0;
}

void cacheCleanup()
{
	// TODO: implement
}


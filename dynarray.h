typedef struct {
	void** objects;
	int len;
	int size;
} DynArray;

int reserveDynArray(DynArray *dynArray, int count);
int addToDynArray(DynArray *dynArray, void* newObject);
void freeDynArray(DynArray *dynArray);
int removeFromDynArrayUnordered(DynArray *dynArray, void* element);
void removeFromDynArrayUnorderedByIndex(DynArray *dynArray, int i);

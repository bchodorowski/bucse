typedef struct {
	void** objects;
	int len;
	int size;
} DynArray;

int addToDynArray(DynArray *dynArray, void* newObject);
void freeDynArray(DynArray *dynArray);
int removeFromDynArrayUnordered(DynArray *dynArray, void* element);

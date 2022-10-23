typedef void (*ActionAddedCalback)(char* filename, char* buf, size_t size);

typedef struct {
	int (*putStorageFile)(char* filename, char *buf, size_t size);
	int (*getStorageFile)(char* filename, char *buf, size_t *size);
	int (*addActionFile)(char* filename, char *buf, size_t size);
	int (*setCallbackActionAdded)(ActionAddedCalback callback);
} Destination;

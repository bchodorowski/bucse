#define FUSE_USE_VERSION 34

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <termios.h>

#include <fuse_lowlevel.h>
#include <fuse.h>

#include <json.h>

#include <pthread.h>

#include "dynarray.h"
#include "filesystem.h"
#include "actions.h"

#include "destinations/dest.h"
#include "encryption/encr.h"

#define PACKAGE_VERSION "current"

// We need a bit larger buffer for decryption due to how EVP_DecryptUpdate() works.
// Quote from https://www.openssl.org/docs/man1.1.1/man3/EVP_DecryptUpdate.html
//
// > The parameters and restrictions are identical to the encryption operations
// > except that if padding is enabled the decrypted data buffer out passed to
// > EVP_DecryptUpdate() should have sufficient room for (inl +
// > cipher_block_size) bytes unless the cipher block size is 1 in which case inl
// > bytes is sufficient. 
#define DECRYPTED_BUFFER_MARGIN 16

static uid_t cachedUid;
static gid_t cachedGid;

static pthread_mutex_t bucseMutex;

struct bucse_config {
	char *repository;
	int verbose;
	char *passphrase;
	char *repositoryRealPath;
};

static struct bucse_config conf;

static void confCleanup() {
	if (conf.repository) {
		free(conf.repository);
		conf.repository = NULL;
	}
	if (conf.repositoryRealPath) {
		free(conf.repositoryRealPath);
		conf.repositoryRealPath = NULL;
	}
	if (conf.passphrase) {
		free(conf.passphrase);
		conf.passphrase = NULL;
	}
}


static int64_t getCurrentTime()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);

	return (int64_t)tv.tv_sec*1000000 + (int64_t)tv.tv_usec;
}

static struct timespec microsecondsToNanoseconds(int64_t t)
{
	struct timespec ts;
	ts.tv_sec = t / 1000000L;
	ts.tv_nsec = (t % 1000000L) * 1000L;
	return ts;
}

typedef struct {
	const char* block;
	off_t offset;
	size_t len;
} BlockOffsetLen;

static int flushFile(FilesystemFile* file);

static void recursivelyFreeFilesystem(FilesystemDir* dir) {
	for (int i=0; i<dir->dirs.len; i++) {
		recursivelyFreeFilesystem(dir->dirs.objects[i]);
	}
	for (int i=0; i<dir->files.len; i++) {
		flushFile((FilesystemFile*)dir->files.objects[i]);
		free(dir->files.objects[i]);
	}
	
	freeDynArray(&dir->dirs);
	freeDynArray(&dir->files);
	free(dir);
}

#define MIN_BLOCK_SIZE 512
#define MAX_BLOCK_SIZE (128 * 1024 * 1024)

// determine block size using a file size
static int getBlockSize(int size)
{
	if (size == 0) {
		return 0;
	}

	// we want block size to be a power of 2,
	// in general, we aim to have between 4 and 8 blocks in a file,
	int result = 1;
	size /= 4;

	while (size) {
		size >>= 1;
		result <<= 1;
	}
	result >>= 1;

	// unless we hit those edge cases
	if (result < MIN_BLOCK_SIZE) {
		return MIN_BLOCK_SIZE;
	}

	if (result > MAX_BLOCK_SIZE) {
		return MAX_BLOCK_SIZE;
	}

	return result;
}

static int determineBlocksToWrite(char *blocksToWrite, off_t offset, size_t size, int fileBlockSize)
{
	while (size > 0) {
		int blockIndex = offset / fileBlockSize;
		off_t blockOffset = (offset % fileBlockSize);
		size_t blockLen = fileBlockSize - blockOffset;
		if (blockLen > size) {
			blockLen = size;
		}

		blocksToWrite[blockIndex] = 1;

		offset += blockLen;
		size -= blockLen;
	}
	
	return 0;
}

// TODO: move this to Encryption[?]
static size_t getMaxEncryptedBlockSize(size_t blockSize)
{
	blockSize *= 2;
	if (blockSize < 256) {
		blockSize = 256;
	}
	return blockSize;
}

extern Destination destinationLocal;
extern Encryption encryptionNone;
extern Encryption encryptionAes;

static Destination *destination;
static Encryption *encryption;
static pthread_t tickThread;

static pthread_mutex_t shutdownMutex;
static int shutdownTicking = 0;

static int getRandomStorageFileName(char* filename)
{
	FILE* f = fopen("/dev/urandom", "rb");
	if (f == NULL) {
		fprintf(stderr, "getRandomStorageFileName: fopen(): %s\n", strerror(errno));
		filename[0] = 0;
		return 1;
	}

	unsigned char buf[20];
	size_t got = fread(buf, 20, 1, f);
	if (got == 0) {
		fprintf(stderr, "getRandomStorageFileName: fread failed\n");
		fclose(f);
		return 2;
	}

	fclose(f);

	for (int i=0; i<20; i++) {
		sprintf(filename + 2*i, "%02x", buf[i]);
	}

	return 0;
}

static int encryptAndAddActionFile(Action* newAction)
{
	char* jsonData = serializeAction(newAction);
	if (jsonData == NULL) {
		fprintf(stderr, "encryptAndAddActionFile: serializeAction() failed\n");
		return -1;
	}

	char newActionFileName[MAX_STORAGE_NAME_LEN];
	if (getRandomStorageFileName(newActionFileName) != 0) {
		fprintf(stderr, "encryptAndAddActionFile: getRandomStorageFileName failed\n");
		free(jsonData);
		return -2;
	}

	size_t encryptedBufLen = 2 * MAX_ACTION_LEN;
	char* encryptedBuf = malloc(encryptedBufLen);
	if (encryptedBuf == NULL) {
		fprintf(stderr, "encryptAndAddActionFile: malloc(): %s\n", strerror(errno));
		free(jsonData);
		return -3;
	}
	int result = encryption->encrypt(jsonData, strlen(jsonData),
		encryptedBuf, &encryptedBufLen,
		conf.passphrase);

	if (result != 0) {
		fprintf(stderr, "encryptAndAddActionFile: encrypt failed: %d\n", result);
		free(jsonData);
		free(encryptedBuf);
		return -4;
	}

	result = destination->addActionFile(newActionFileName, encryptedBuf, encryptedBufLen);
	free(jsonData);
	free(encryptedBuf);
	return result;
}

static void actionAddedDecrypt(char* actionName, char* buf, size_t size, int moreInThisBatch)
{
	size_t decryptedBufLen = MAX_ACTION_LEN + DECRYPTED_BUFFER_MARGIN;
	char* decryptedBuf = malloc(decryptedBufLen);
	if (decryptedBuf == NULL) {
		fprintf(stderr, "decryptAndAddActionFile: malloc(): %s\n", strerror(errno));
		return;
	}

	int result = encryption->decrypt(buf, size,
		decryptedBuf, &decryptedBufLen,
		conf.passphrase);
	
	if (result != 0) {
		fprintf(stderr, "actionAddedDecrypt: decrypt failed: %d\n", result);
		free(decryptedBuf);
		return;
	}
	actionAdded(actionName, decryptedBuf, decryptedBufLen, moreInThisBatch);
	free(decryptedBuf);
}


static int flushFile(FilesystemFile* file)
{
	if (file->dirtyFlags == DirtyFlagNotDirty) {
		return 0;
	}
	// TODO: what if create only, no writes?

	int newSize = file->size;
	if (file->dirtyFlags & DirtyFlagPendingTrunc) {
		newSize = file->truncSize;
		file->truncSize = 0;
	}
	int newBlockSize = file->blockSize;
	int newContentLen = file->contentLen;
	char* newContent = NULL;

	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		if (newSize < (pw->offset + pw->size)) {
			newSize = (pw->offset + pw->size);
		}
	}

	// block size may not be determined yet if the file hasn't been flushed
	// with any data
	if (file->blockSize == 0) {
		newBlockSize = getBlockSize(newSize);
	}

	if (newBlockSize == 0) {
		newContentLen = 0;
	} else {
		newContentLen = newSize / newBlockSize + (int)(newSize % newBlockSize != 0);
	}

	// TODO: if newContentLen is big, consider changing the blockSize and
	//       rewriting the whole file

	if (file->pendingWrites.len == 0) {
		if (newContentLen > 0) {
			newContent = malloc(newContentLen * MAX_STORAGE_NAME_LEN);
			if (newContent == NULL) {
				fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
				return 1;
			}
			memcpy(newContent, file->content, newContentLen * MAX_STORAGE_NAME_LEN);
		}

		goto constructAction;
	}

	// determine which blocks have been changed -- one byte per block
	char* blocksToWrite = malloc(newContentLen);
	if (blocksToWrite == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		return 1;
	}
	memset(blocksToWrite, 0, newContentLen);

	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		determineBlocksToWrite(blocksToWrite, pw->offset, pw->size, newBlockSize);
	}

	// debug: print number of blocks to be written
	int blocksToWriteNum = 0;
	for (int i=0; i<newContentLen; i++) {
		blocksToWriteNum += blocksToWrite[i];
	}
	fprintf(stderr, "DEBUG: flush file: %d blocks to write\n", blocksToWriteNum);

	// construct and save new blocks (with destination->putStorageFile() calls)
	size_t maxEncryptedBlockSize = getMaxEncryptedBlockSize(newBlockSize);
	char* encryptedBlockBuf = malloc(maxEncryptedBlockSize);
	if (encryptedBlockBuf == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		free(blocksToWrite);
		return 2;
	}

	size_t maxDecryptedBlockSize = newBlockSize;
	char* decryptedBlockBuf = malloc(maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN);
	if (decryptedBlockBuf == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		free(blocksToWrite);
		free(encryptedBlockBuf);
		return 3;
	}
	newContent = malloc(newContentLen * MAX_STORAGE_NAME_LEN);
	if (newContent == NULL) {
		free(blocksToWrite);
		free(encryptedBlockBuf);
		free(decryptedBlockBuf);
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		return 4;
	}
	int ioerror = 0;
	for (int i=0; i<newContentLen; i++) {
		if (blocksToWrite[i] == 0) {
			memcpy(newContent + (MAX_STORAGE_NAME_LEN * i),
				file->content + (MAX_STORAGE_NAME_LEN * i),
				MAX_STORAGE_NAME_LEN);
			continue;
		}

		size_t encryptedBlockBufSize = maxEncryptedBlockSize;
		size_t decryptedBlockBufSize = maxDecryptedBlockSize;
		if (i < file->contentLen) {
			size_t expectedReadSize = file->size - (i * file->blockSize);
			if (expectedReadSize > file->blockSize) {
				expectedReadSize = file->blockSize;
			}

			const char* block = file->content + (MAX_STORAGE_NAME_LEN * i);
			int res = destination->getStorageFile(block, encryptedBlockBuf, &encryptedBlockBufSize);
			if (res != 0) {
				fprintf(stderr, "flushFile: getStorageFile failed for %s: %d\n",
					block, res);
				ioerror = 1;
				break;
			}

			res = encryption->decrypt(encryptedBlockBuf, encryptedBlockBufSize,
				decryptedBlockBuf, &decryptedBlockBufSize,
				conf.passphrase);
			if (res != 0) {
				fprintf(stderr, "flushFile: decrypt failed: %d\n", res);
				ioerror = 1;
				break;
			}
			if (decryptedBlockBufSize != expectedReadSize) {
				fprintf(stderr, "flushFile: expected decrypted block size %d, got %d\n",
					expectedReadSize, decryptedBlockBufSize);
				ioerror = 1;
				break;
			}
		}
		// right now we have data in decryptedBlockBuf
		
		// apply write operations
		for (int j=0; j<file->pendingWrites.len; j++) {
			PendingWrite* pw = file->pendingWrites.objects[j];
			int relOffset = pw->offset - i * newBlockSize;

			if (relOffset >= newBlockSize) {
				continue;
			} else if (relOffset + (int)pw->size <= 0) {
				continue;
			}

			if (relOffset <= 0) {
				int bytesToCopy = pw->size + relOffset;
				if (bytesToCopy > newBlockSize) {
					bytesToCopy = newBlockSize;
				}
				memcpy(decryptedBlockBuf, pw->buf - relOffset, bytesToCopy);
			} else {
				int bytesToCopy = pw->size;
				if (bytesToCopy > newBlockSize - relOffset) {
					bytesToCopy = newBlockSize - relOffset;
				}
				memcpy(decryptedBlockBuf + relOffset, pw->buf, bytesToCopy);
			}
		}

		size_t expectedWriteSize = newSize - (i * newBlockSize);
		if (expectedWriteSize > newBlockSize) {
			expectedWriteSize = newBlockSize;
		}
		encryptedBlockBufSize = maxEncryptedBlockSize;

		// encrypt
		int res = encryption->encrypt(decryptedBlockBuf, expectedWriteSize,
			encryptedBlockBuf, &encryptedBlockBufSize,
			conf.passphrase);
		if (res != 0) {
			fprintf(stderr, "flushFile: encrypt failed: %d\n", res);
			ioerror = 1;
			break;
		}

		// save
		char newStorageFileName[MAX_STORAGE_NAME_LEN];
		if (getRandomStorageFileName(newStorageFileName) != 0) {
			fprintf(stderr, "flushFile: getRandomStorageFileName failed\n");
			ioerror = 1;
			break;
		}
		res = destination->putStorageFile(newStorageFileName, encryptedBlockBuf, encryptedBlockBufSize);
		if (res != 0) {
			fprintf(stderr, "flushFile: putStorageFile failed: %d\n", res);
			ioerror = 1;
			break;
		}

		memcpy(newContent + (MAX_STORAGE_NAME_LEN * i),
			newStorageFileName,
			MAX_STORAGE_NAME_LEN);
	}

	free(encryptedBlockBuf);
	free(decryptedBlockBuf);
	free(blocksToWrite);

	if (ioerror) {
		if (newContent) {
			free(newContent);
		}
		return 5;
	}

constructAction:;

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		fprintf(stderr, "flushFile: malloc(): %s\n", strerror(errno));
		if (newContent) {
			free(newContent);
		}
		return 6;
	}
	newAction->time = getCurrentTime();

	if (file->dirtyFlags == DirtyFlagPendingWrite) {
		newAction->actionType = ActionTypeEditFile;
	} else if (file->dirtyFlags & DirtyFlagPendingCreate) {
		newAction->actionType = ActionTypeAddFile;
	}

	newAction->path = getFullFilePath(file);
	if (newAction->path == NULL) {
		fprintf(stderr, "flushFile: getFullFilePath() failed: %s\n", strerror(errno));
		if (newContent) {
			free(newContent);
		}
		free(newAction);
		return 7;
	}
	newAction->content = newContent;
	newAction->contentLen = newContentLen;
	newAction->size = newSize;
	newAction->blockSize = newBlockSize;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		fprintf(stderr, "flushFile: encryptAndAddActionFile failed\n");
		if (newContent) {
			free(newContent);
		}
		free(newAction->path);
		free(newAction);
		return 10;
	}

	// add to actions array
	addAction(newAction);

	// update file
	if (file->dirtyFlags & DirtyFlagPendingCreate) {
		free((char*)file->name);
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		file->name = path_split(newAction->path, &pathArray);
		path_free(&pathArray);
	}
	file->mtime = newAction->time;
	file->content = newAction->content;
	file->contentLen = newAction->contentLen;
	file->size = newAction->size;
	file->blockSize = newAction->blockSize;
	file->dirtyFlags = DirtyFlagNotDirty;

	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		free(pw->buf);
		free(pw);
	}
	freeDynArray(&file->pendingWrites);

	return 0;
}

static int bucse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	(void) fi;

	if (path == NULL) {
		return -EIO;
	}

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;

		stbuf->st_atim = microsecondsToNanoseconds(root->atime);
		stbuf->st_mtim = microsecondsToNanoseconds(root->mtime);
		stbuf->st_ctim = microsecondsToNanoseconds(root->mtime);

		stbuf->st_uid = cachedUid;
		stbuf->st_gid = cachedGid;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_getattr: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			if (file->dirtyFlags & DirtyFlagPendingWrite) {
				flushFile(file);
			}

			stbuf->st_mode = S_IFREG | 0644;
			stbuf->st_nlink = 1;
			stbuf->st_size = (file->dirtyFlags & DirtyFlagPendingTrunc)
				? file->truncSize
				: file->size;
			//stbuf->st_ino = MurmurHash64(path, strlen(path), 0);
			stbuf->st_atim = microsecondsToNanoseconds(file->atime);
			stbuf->st_mtim = microsecondsToNanoseconds(file->mtime);
			stbuf->st_ctim = microsecondsToNanoseconds(file->mtime);

			stbuf->st_uid = cachedUid;
			stbuf->st_gid = cachedGid;

		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 1;
				//stbuf->st_ino = MurmurHash64(path, strlen(path), 0);
				stbuf->st_atim = microsecondsToNanoseconds(dir->atime);
				stbuf->st_mtim = microsecondsToNanoseconds(dir->mtime);
				stbuf->st_ctim = microsecondsToNanoseconds(dir->mtime);

				stbuf->st_uid = cachedUid;
				stbuf->st_gid = cachedGid;
			} else {
				return -ENOENT;
			}
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int bucse_getattr_guarded(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_getattr(path, stbuf, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	(void) offset;
	(void) fi;
	(void) flags;

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *dir;

	if (strcmp(path, "/") == 0) {
		dir = root;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_readdir: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		dir = findDirByPath(&pathArray);
		path_free(&pathArray);
	} else {
		return -ENOENT;
	}

	if (dir == NULL) {
		return -ENOENT;
	}

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	for (int i=0; i<dir->dirs.len; i++) {
		FilesystemDir* d = dir->dirs.objects[i];
		filler(buf, d->name, NULL, 0, 0);
	}

	for (int i=0; i<dir->files.len; i++) {
		FilesystemFile* f = dir->files.objects[i];
		filler(buf, f->name, NULL, 0, 0);
	}
	dir->atime = getCurrentTime();
	return 0;
}

static int bucse_readdir_guarded(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_readdir(path, buf, filler, offset, fi, flags);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_open(const char *path, struct fuse_file_info *fi)
{
	fprintf(stderr, "DEBUG: open %s, access mode %d\n", path, fi->flags);

	if (path == NULL) {
		return -EIO;
	}

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_open: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_open: path not found when opening file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			if ((fi->flags & O_CREAT) && (fi->flags & O_EXCL)) {
				return -EEXIST;
			}

			if (fi->flags & O_TRUNC) {
				file->truncSize = 0;
				file->dirtyFlags |= DirtyFlagPendingTrunc;
			}

			return 0;
		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				if (fi->flags & O_CREAT) {
					FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
					if (newFile == NULL) {
						fprintf(stderr, "bucse_open: malloc(): %s\n", strerror(errno));
						return -ENOMEM;
					}

					newFile->name = strdup(fileName);
					if (newFile->name == NULL) {
						fprintf(stderr, "splitPath: strdup(): %s\n", strerror(errno));
						free(newFile);
						return -ENOMEM;
					}
					newFile->atime = newFile->mtime = getCurrentTime();
					newFile->content = NULL;
					newFile->contentLen = 0;
					newFile->size = 0;
					newFile->blockSize = 0;
					newFile->dirtyFlags = DirtyFlagPendingCreate;
					memset(&newFile->pendingWrites, 0, sizeof(DynArray));
					newFile->parentDir = containingDir;

					addToDynArray(&containingDir->files, newFile);
					return 0;
				}
				return -ENOENT;
			}
		}
	} else {
		return -ENOENT;
	}
}

static int bucse_open_guarded(const char *path, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_open(path, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	fprintf(stderr, "DEBUG: create %s, mode %d, access mode %d\n", path, mode, fi->flags);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *containingDir = NULL;
	const char *fileName = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_create: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);
		if (containingDir == NULL) {
			fprintf(stderr, "bucse_create: path not found when creating file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			if ((fi->flags & O_CREAT) && (fi->flags & O_EXCL)) {
				return -EEXIST;
			}
			return bucse_open(path, fi);
		} else {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EEXIST;
			} else {
				// continue below
			}
		}
	} else {
		return -ENOENT;
	}

	FilesystemFile* newFile = malloc(sizeof(FilesystemFile));
	if (newFile == NULL) {
		fprintf(stderr, "bucse_create: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}

	newFile->name = strdup(fileName);
	if (newFile->name == NULL) {
		fprintf(stderr, "splitPath: strdup(): %s\n", strerror(errno));
		free(newFile);
		return -ENOMEM;
	}
	newFile->atime = newFile->mtime = getCurrentTime();
	newFile->content = NULL;
	newFile->contentLen = 0;
	newFile->size = 0;
	newFile->blockSize = 0;
	newFile->dirtyFlags = DirtyFlagPendingCreate;
	memset(&newFile->pendingWrites, 0, sizeof(DynArray));
	newFile->parentDir = containingDir;

	addToDynArray(&containingDir->files, newFile);
	return 0;
}

static int bucse_create_guarded(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_create(path, mode, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_release(const char *path, struct fuse_file_info *fi)
{
	fprintf(stderr, "DEBUG: release %s, access mode %d\n", path, fi->flags);

	if (path == NULL) {
		return -EIO;
	}

	if ((fi->flags & O_ACCMODE) == O_RDONLY) {
		return 0;
	}

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_release: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_release: path not found when releasing file %s\n", path);
			return -ENOENT;
		}

		FilesystemFile *file = findFile(containingDir, fileName);
		if (file) {
			flushFile(file);
		}
	} else {
		return -ENOENT;
	}

	return 0;
}
static int bucse_release_guarded(const char *path, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_release(path, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

// use offset and size to determine which blocks contain the data that's needed
static int determineBlocksToRead(DynArray *blocksToRead, off_t offset, size_t size, FilesystemFile* file)
{
	if (offset > file->size) {
		return 0;
	}
	if (size > file->size - offset) {
		size = file->size - offset;
	}

	while (size > 0) {
		int blockIndex = offset / file->blockSize;
		off_t blockOffset = (offset % file->blockSize);
		size_t blockLen = file->blockSize - blockOffset;
		if (blockLen > size) {
			blockLen = size;
		}
		BlockOffsetLen *block = malloc(sizeof(BlockOffsetLen));
		if (!block) {
			fprintf(stderr, "determineBlocksToRead: malloc(): %s\n", strerror(errno));
			return 1;
		}
		block->block = file->content + MAX_STORAGE_NAME_LEN * blockIndex;
		block->offset = blockOffset;
		block->len = blockLen;
		addToDynArray(blocksToRead, block);

		offset += blockLen;
		size -= blockLen;
	}
	
	return 0;
}

static int bucse_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	fprintf(stderr, "DEBUG: read %s, size: %u, offset: %u\n", path, size, offset);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_read: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_read: path not found when reading file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	if (file->dirtyFlags != DirtyFlagNotDirty) {
		flushFile(file);
	}

	// determine which blocks should be read
	DynArray blocksToRead;
	memset(&blocksToRead, 0, sizeof(DynArray));
	if (determineBlocksToRead(&blocksToRead, offset, size, file) != 0) {
		fprintf(stderr, "bucse_read: determineBlocksToRead failed\n");
		for (int i=0; i<blocksToRead.len; i++) {
			free(blocksToRead.objects[i]);
		}
		freeDynArray(&blocksToRead);
		return -ENOMEM;
	}

	size_t maxEncryptedBlockSize = getMaxEncryptedBlockSize(file->blockSize);
	char* encryptedBlockBuf = malloc(maxEncryptedBlockSize);
	if (encryptedBlockBuf == NULL) {
		fprintf(stderr, "bucse_read: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}

	size_t maxDecryptedBlockSize = file->blockSize;
	char* decryptedBlockBuf = malloc(maxDecryptedBlockSize + DECRYPTED_BUFFER_MARGIN);
	if (decryptedBlockBuf == NULL) {
		fprintf(stderr, "bucse_read: malloc(): %s\n", strerror(errno));
		free(encryptedBlockBuf);
		return -ENOMEM;
	}

	size_t copiedBytes = 0;
	int ioerror = 0;
	for (int i=0; i<blocksToRead.len; i++) {
		BlockOffsetLen* block = blocksToRead.objects[i];
		size_t encryptedBlockBufSize = maxEncryptedBlockSize;
		int res = destination->getStorageFile(block->block, encryptedBlockBuf, &encryptedBlockBufSize);
		if (res != 0) {
			fprintf(stderr, "bucse_read: getStorageFile failed for %s: %d\n",
				block->block, res);
			ioerror = 1;
			break;
		}

		size_t decryptedBlockBufSize = maxDecryptedBlockSize;
		res = encryption->decrypt(encryptedBlockBuf, encryptedBlockBufSize,
			decryptedBlockBuf, &decryptedBlockBufSize,
			conf.passphrase);
		if (res != 0) {
			fprintf(stderr, "bucse_read: decrypt failed: %d\n", res);
			ioerror = 1;
			break;
		}
		if (decryptedBlockBufSize < block->offset + block->len) {
			fprintf(stderr, "bucse_read: expected decrypted block size at least %d, got %d\n",
				block->offset + block->len, decryptedBlockBufSize);
			ioerror = 1;
			break;
		}

		memcpy(buf + copiedBytes, decryptedBlockBuf + block->offset, block->len);
		copiedBytes += block->len;
	}

	free(encryptedBlockBuf);
	free(decryptedBlockBuf);

	for (int i=0; i<blocksToRead.len; i++) {
		free(blocksToRead.objects[i]);
	}
	freeDynArray(&blocksToRead);

	if (ioerror) {
		return -EIO;
	}

	file->atime = getCurrentTime();
	return copiedBytes;
}

static int bucse_read_guarded(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_read(path, buf, size, offset, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_write(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;

	fprintf(stderr, "DEBUG: write %s, size: %u, offset: %u\n", path, size, offset);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_write: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_write: path not found when writing file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	PendingWrite* newPendingWrite = malloc(sizeof(PendingWrite));
	if (newPendingWrite == NULL) {
		fprintf(stderr, "bucse_write: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newPendingWrite->size = size;
	newPendingWrite->buf = malloc(size);
	if (newPendingWrite->buf == NULL) {
		fprintf(stderr, "bucse_write: malloc(): %s\n", strerror(errno));
		free(newPendingWrite);
		return -ENOMEM;
	}
	memcpy(newPendingWrite->buf, buf, size);
	newPendingWrite->offset = offset;
	addToDynArray(&file->pendingWrites, newPendingWrite);
	file->dirtyFlags |= DirtyFlagPendingWrite;

	// TODO: flush file if pendingWrites are too large

	return size;
}

static int bucse_write_guarded(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_write(path, buf, size, offset, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_unlink(const char *path)
{
	fprintf(stderr, "DEBUG: unlink %s\n", path);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;
	FilesystemDir *containingDir = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_unlink: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_unlink: path not found when deleting file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		fprintf(stderr, "bucse_unlink: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getCurrentTime();
	newAction->actionType = ActionTypeRemoveFile;

	newAction->path = getFullFilePath(file);
	if (newAction->path == NULL) {
		fprintf(stderr, "bucse_unlink: getFullFilePath() failed: %s\n", strerror(errno));
		free(newAction);
		return -ENOMEM;
	}
	newAction->content = NULL;
	newAction->contentLen = 0;
	newAction->size = 0;
	newAction->blockSize = 0;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		fprintf(stderr, "bucse_unlink: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		return -EIO;
	}

	// add to actions array
	addAction(newAction);

	// update filesystem
	if (removeFromDynArrayUnordered(&containingDir->files, (void*)file) != 0) {
		fprintf(stderr, "bucse_unlink: removeFromDynArrayUnordered() failed\n");
		return -EIO;
	}
	free(file);

	return 0;
}

static int bucse_unlink_guarded(const char *path)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_unlink(path);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_mkdir(const char *path, mode_t mode)
{
	(void) mode;

	fprintf(stderr, "DEBUG: mkdir %s\n", path);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *containingDir = NULL;
	const char *dirName = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		dirName = path_split(path+1, &pathArray);
		if (dirName == NULL) {
			fprintf(stderr, "bucse_mkdir: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_mkdir: path not found when adding directory %s\n", path);
			return -ENOENT;
		}

		FilesystemDir *dir = findDir(containingDir, dirName);
		if (dir != NULL) {
			return -EEXIST;
		}
		FilesystemFile* file = findFile(containingDir, dirName);
		if (file != NULL) {
			return -EEXIST;
		}
	} else {
		return -ENOENT;
	}

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		fprintf(stderr, "bucse_mkdir: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getCurrentTime();
	newAction->actionType = ActionTypeAddDirectory;

	newAction->path = strdup(path+1);
	if (newAction->path == NULL) {
		fprintf(stderr, "bucse_mkdir: strdup() failed: %s\n", strerror(errno));
		free(newAction);
		return -ENOMEM;
	}
	newAction->content = NULL;
	newAction->contentLen = 0;
	newAction->size = 0;
	newAction->blockSize = 0;

	FilesystemDir* newDir = malloc(sizeof(FilesystemDir));
	if (newDir == NULL) {
		fprintf(stderr, "bucse_mkdir: malloc(): %s\n", strerror(errno));
		free(newAction->path);
		free(newAction);
		return -ENOMEM;
	}
	memset(newDir, 0, sizeof(FilesystemDir));

	// funny way to find the const char* filename that's owned by the action
	DynArray pathArray;
	memset(&pathArray, 0, sizeof(DynArray));
	newDir->name = path_split(newAction->path, &pathArray);
	path_free(&pathArray);

	newDir->atime = newDir->mtime = newAction->time;
	newDir->parentDir = containingDir;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		fprintf(stderr, "bucse_mkdir: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		free(newDir);
		return -EIO;
	}

	// add to actions array
	addAction(newAction);

	// update filesystem
	addToDynArray(&containingDir->dirs, newDir);

	return 0;
}

static int bucse_mkdir_guarded(const char *path, mode_t mode)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_mkdir(path, mode);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_rmdir(const char *path)
{
	fprintf(stderr, "DEBUG: rmdir %s\n", path);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemDir *containingDir = NULL;
	const char *dirName = NULL;
	FilesystemDir *dir = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		dirName = path_split(path+1, &pathArray);
		if (dirName == NULL) {
			fprintf(stderr, "bucse_rmdir: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_rmdir: path not found when adding directory %s\n", path);
			return -ENOENT;
		}

		dir = findDir(containingDir, dirName);
		if (dir == NULL) {
			FilesystemFile* file = findFile(containingDir, dirName);
			if (file != NULL) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the dir below
	} else {
		return -ENOENT;
	}

	if (dir->files.len > 0 || dir->dirs.len > 0) {
		fprintf(stderr, "bucse_rmdir: can't delete directory %s because it is not empty\n", path+1);
		return -ENOTEMPTY;
	}

	// construct new action, add it to actions
	Action* newAction = malloc(sizeof(Action));
	if (newAction == NULL) {
		fprintf(stderr, "bucse_rmdir: malloc(): %s\n", strerror(errno));
		return -ENOMEM;
	}
	newAction->time = getCurrentTime();
	newAction->actionType = ActionTypeRemoveDirectory;

	newAction->path = strdup(path+1);
	if (newAction->path == NULL) {
		fprintf(stderr, "bucse_rmdir: strdup() failed: %s\n", strerror(errno));
		free(newAction);
		return -ENOMEM;
	}
	newAction->content = NULL;
	newAction->contentLen = 0;
	newAction->size = 0;
	newAction->blockSize = 0;

	// write to json, encrypt call destination->addActionFile()
	if (encryptAndAddActionFile(newAction) != 0) {
		fprintf(stderr, "bucse_rmdir: encryptAndAddActionFile failed\n");
		free(newAction->path);
		free(newAction);
		return -EIO;
	}

	// add to actions array
	addAction(newAction);

	// update filesystem
	if (removeFromDynArrayUnordered(&containingDir->dirs, (void*)dir) != 0) {
		fprintf(stderr, "bucse_unlink: removeFromDynArrayUnordered() failed\n");
		return -EIO;
	}
	freeDynArray(&dir->dirs);
	freeDynArray(&dir->files);
	free(dir);

	return 0;
}

static int bucse_rmdir_guarded(const char *path)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_rmdir(path);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_truncate(const char *path, long int newSize, struct fuse_file_info *fi)
{
	(void) fi;

	fprintf(stderr, "DEBUG: truncate %s, size: %ld\n", path, newSize);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_truncate: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_truncate: path not found when writing file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	if (file->dirtyFlags != DirtyFlagNotDirty) {
		flushFile(file);
	}

	int size = file->size;
	for (int i=0; i<file->pendingWrites.len; i++) {
		PendingWrite* pw = file->pendingWrites.objects[i];
		if (size < (pw->offset + pw->size)) {
			size = (pw->offset + pw->size);
		}
	}

	if (newSize == size) {
		return 0;
	} else if (newSize > size) {
		PendingWrite* newPendingWrite = malloc(sizeof(PendingWrite));
		if (newPendingWrite == NULL) {
			fprintf(stderr, "bucse_truncate: malloc(): %s\n", strerror(errno));
			return -ENOMEM;
		}
		newPendingWrite->size = newSize - size;
		newPendingWrite->buf = malloc(newSize - size);
		if (newPendingWrite->buf == NULL) {
			fprintf(stderr, "bucse_truncate: malloc(): %s\n", strerror(errno));
			free(newPendingWrite);
			return -ENOMEM;
		}
		memset(newPendingWrite->buf, 0, newSize - size);
		newPendingWrite->offset = size;
		addToDynArray(&file->pendingWrites, newPendingWrite);
		file->dirtyFlags |= DirtyFlagPendingWrite;
		return 0;
	}

	// (newSize < size), so
	
	file->truncSize = newSize;
	file->dirtyFlags |= DirtyFlagPendingWrite;

	return 0;
}

static int bucse_truncate_guarded(const char *path, long int newSize, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_truncate(path, newSize, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static int bucse_flush(const char *path, struct fuse_file_info *fi)
{
	(void) fi;

	fprintf(stderr, "DEBUG: flush %s\n", path);

	if (path == NULL) {
		return -EIO;
	}

	FilesystemFile *file = NULL;

	if (strcmp(path, "/") == 0) {
		return -EACCES;
	} else if (path[0] == '/') {
		DynArray pathArray;
		memset(&pathArray, 0, sizeof(DynArray));
		const char *fileName = path_split(path+1, &pathArray);
		if (fileName == NULL) {
			fprintf(stderr, "bucse_flush: path_split() failed\n");
			return -ENOMEM;
		}
		//path_debugPrint(&pathArray);

		FilesystemDir *containingDir = findContainingDir(&pathArray);
		path_free(&pathArray);

		if (containingDir == NULL) {
			fprintf(stderr, "bucse_flush: path not found when writing file %s\n", path);
			return -ENOENT;
		}

		file = findFile(containingDir, fileName);
		if (file == NULL) {
			FilesystemDir* dir = findDir(containingDir, fileName);
			if (dir) {
				return -EACCES;
			} else {
				return -ENOENT;
			}
		}
		// continue working with the file below
	} else {
		return -ENOENT;
	}

	if (file->dirtyFlags & DirtyFlagPendingWrite) {
		flushFile(file);
	}

	return 0;
}

static int bucse_flush_guarded(const char *path, struct fuse_file_info *fi)
{
	pthread_mutex_lock(&bucseMutex);
	int result = bucse_flush(path, fi);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

static void* bucse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	//cfg->use_ino = 1;
	cfg->hard_remove = 1;
}

static void* bucse_init_guarded(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	pthread_mutex_lock(&bucseMutex);
	void* result = bucse_init(conn, cfg);
	pthread_mutex_unlock(&bucseMutex);
	return result;
}

struct fuse_operations bucse_oper = {
	.getattr = bucse_getattr_guarded,
	.open = bucse_open_guarded,
	.create = bucse_create_guarded,
	.release = bucse_release_guarded,
	.read = bucse_read_guarded,
	.readdir = bucse_readdir_guarded,
	.write = bucse_write_guarded,
	.unlink = bucse_unlink_guarded,
	.mkdir = bucse_mkdir_guarded,
	.rmdir = bucse_rmdir_guarded,
	.truncate = bucse_truncate_guarded,
	.flush = bucse_flush_guarded,
	.init = bucse_init_guarded,
};

enum {
	KEY_HELP,
	KEY_VERSION,
};

#define BUCSE_OPT(t, p, v) { t, offsetof(struct bucse_config, p), v }

static struct fuse_opt bucse_opts[] = {
	BUCSE_OPT("repository=%s", repository, 0),
	BUCSE_OPT("-r %s", repository, 0),
	BUCSE_OPT("verbose=%d", verbose, 0),
	BUCSE_OPT("-v %d", verbose, 0),
	BUCSE_OPT("passphrase=%s", passphrase, 0),
	BUCSE_OPT("-p %s", passphrase, 0),

	FUSE_OPT_KEY("-V",             KEY_VERSION),
	FUSE_OPT_KEY("--version",      KEY_VERSION),
	FUSE_OPT_KEY("-h",             KEY_HELP),
	FUSE_OPT_KEY("--help",         KEY_HELP),
	FUSE_OPT_END
};

static int bucse_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	switch (key) {
	case KEY_HELP:
		fuse_opt_add_arg(outargs, "-h");
		fuse_main(outargs->argc, outargs->argv, &bucse_oper, NULL);
		fprintf(stderr,
				"\n"
				"bucse options:\n"
				"    -o repository=STRING   TODO\n"
				"    -r STRING              same as '-orepository=STRING'\n"
				"    -o verbose=INTEGER     TODO\n"
				"    -v INTEGER             same as '-overbose=INTEGER'\n"
				"    -o passphrase=STRING   TODO\n"
				"    -p STRING              same as '-opassphrase=STRING'\n"
				, outargs->argv[0]);
		exit(1);

	case KEY_VERSION:
		fprintf(stderr, "bucse version %s\n", PACKAGE_VERSION);
		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &bucse_oper, NULL);
		exit(0);
	}
	return 1;
}

void* tickThreadFunc(void* param)
{
	printf("DEBUG: Hello from tickThreadFunc\n");
	for (;;) {
		pthread_mutex_lock(&shutdownMutex);
		if (shutdownTicking) {
			pthread_mutex_unlock(&shutdownMutex);
			break;
		}
		pthread_mutex_unlock(&shutdownMutex);

		pthread_mutex_lock(&bucseMutex);
		int tickResult = destination->tick();
		pthread_mutex_unlock(&bucseMutex);

		if (tickResult != 0) {
			break;
		}
		sleep(1);
	}
	return 0;
}

int bucse_fuse_main(int argc, char *argv[], const struct fuse_operations *op,
		size_t op_size, void *user_data)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse *fuse;
	struct fuse_cmdline_opts opts;
	int res;
	struct fuse_loop_config config;

	if (fuse_parse_cmdline(&args, &opts) != 0)
		return 1;

	if (opts.show_version) {
		printf("FUSE library version %s\n", PACKAGE_VERSION);
		fuse_lowlevel_version();
		res = 0;
		goto out1;
	}

	if (opts.show_help) {
		if(args.argv[0][0] != '\0')
			printf("usage: %s [options] <mountpoint>\n\n",
					args.argv[0]);
		printf("FUSE options:\n");
		fuse_cmdline_help();
		fuse_lib_help(&args);
		res = 0;
		goto out1;
	}

	if (!opts.show_help &&
			!opts.mountpoint) {
		fuse_log(FUSE_LOG_ERR, "error: no mountpoint specified\n");
		res = 2;
		goto out1;
	}

	fuse = fuse_new(&args, op, op_size, user_data);
	if (fuse == NULL) {
		res = 3;
		goto out1;
	}

	if (fuse_mount(fuse,opts.mountpoint) != 0) {
		res = 4;
		goto out2;
	}

	if (fuse_daemonize(opts.foreground) != 0) {
		res = 5;
		goto out3;
	}

	// initialize destination thread
	if (destination->isTickable())
	{
		int ret = pthread_create(&tickThread, NULL, tickThreadFunc, NULL);
	}

	struct fuse_session *se = fuse_get_session(fuse);
	if (fuse_set_signal_handlers(se) != 0) {
		res = 6;
		goto out3;
	}

	if (opts.singlethread)
		res = fuse_loop(fuse);
	else {
		config.clone_fd = opts.clone_fd;
		config.max_idle_threads = opts.max_idle_threads;
		res = fuse_session_loop_mt(se, &config);
	}
	if (res)
		res = 8;

	fuse_remove_signal_handlers(se);
out3:
	fuse_unmount(fuse);
out2:
	fuse_destroy(fuse);
out1:
	free(opts.mountpoint);
	fuse_opt_free_args(&args);
	return res;
}

#define MAX_REPOSITORY_JSON_LEN (1024 * 1024)
int parseRepositoryJsonFile() {
	char* repositoryJsonFileContents = malloc(MAX_REPOSITORY_JSON_LEN);
	if (repositoryJsonFileContents == NULL)
	{
		fprintf(stderr, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	size_t repositoryJsonFileLen = MAX_REPOSITORY_JSON_LEN;

	int err = destination->getRepositoryJsonFile(repositoryJsonFileContents, &repositoryJsonFileLen);
	if (err != 0)
	{
		fprintf(stderr, "destination->getRepositoryJsonFile(): %d\n", err);

		free(repositoryJsonFileContents);
		return 2;
	}

	json_tokener* tokener = json_tokener_new();
	json_object* repositoryJson = json_tokener_parse_ex(tokener, repositoryJsonFileContents, repositoryJsonFileLen);

	free(repositoryJsonFileContents);
	json_tokener_free(tokener);

	if (repositoryJson == NULL)
	{
		fprintf(stderr, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		return 3;
	}

	json_object* encryptionField;
	if (json_object_object_get_ex(repositoryJson, "encryption", &encryptionField) == 0)
	{
		fprintf(stderr, "repository object doesn't have 'encryption' field\n");
		json_object_put(repositoryJson);
		return 4;
	}

	if (json_object_get_type(encryptionField) != json_type_string)
	{
		fprintf(stderr, "'encryption' field is not a string\n");
		json_object_put(repositoryJson);
		return 5;
	}

	const char* encryptionFieldStr = json_object_get_string(encryptionField);

	if (strcmp(encryptionFieldStr, "none") == 0) {
		encryption = &encryptionNone;
	} else if (strcmp(encryptionFieldStr, "aes") == 0) {
		encryption = &encryptionAes;
	} else {
		fprintf(stderr, "Unsupported encryption: %s\n", encryptionFieldStr);
		json_object_put(repositoryJson);
		return 6;
	}

	json_object_put(repositoryJson);
	return 0;
}

#define MAX_REPOSITORY_LEN (1024 * 1024)
int parseRepositoryFile() {
	char* repositoryFileContents = malloc(MAX_REPOSITORY_LEN);
	if (repositoryFileContents == NULL)
	{
		fprintf(stderr, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	size_t repositoryFileLen = MAX_REPOSITORY_LEN;

	int err = destination->getRepositoryFile(repositoryFileContents, &repositoryFileLen);
	if (err != 0)
	{
		fprintf(stderr, "destination->getRepositoryFile(): %d\n", err);

		free(repositoryFileContents);
		return 2;
	}

	size_t decryptedBufLen = MAX_REPOSITORY_LEN + DECRYPTED_BUFFER_MARGIN;
	char* decryptedBuf = malloc(decryptedBufLen);
	if (decryptedBuf == NULL) {
		fprintf(stderr, "parseRepositoryFile: malloc(): %s\n", strerror(errno));

		free(repositoryFileContents);
		return 3;
	}

	int result = encryption->decrypt(repositoryFileContents, repositoryFileLen,
		decryptedBuf, &decryptedBufLen,
		conf.passphrase);
	
	if (result != 0) {
		fprintf(stderr, "parseRepositoryFile: decrypt failed: %d\n", result);

		free(repositoryFileContents);
		free(decryptedBuf);
		return 4;
	}

	json_tokener* tokener = json_tokener_new();
	json_object* repository = json_tokener_parse_ex(tokener, decryptedBuf, decryptedBufLen);

	free(repositoryFileContents);
	free(decryptedBuf);
	json_tokener_free(tokener);

	if (repository == NULL)
	{
		fprintf(stderr, "json_tokener_parse_ex(): %s\n", json_tokener_error_desc(json_tokener_get_error(tokener)));
		return 5;
	}

	json_object* timeField;
	if (json_object_object_get_ex(repository, "time", &timeField) == 0)
	{
		fprintf(stderr, "repository object doesn't have 'time' field\n");
		json_object_put(repository);
		return 6;
	}

	if (json_object_get_type(timeField) != json_type_int)
	{
		fprintf(stderr, "'time' field is not a string\n");
		json_object_put(repository);
		return 7;
	}
	int64_t time = json_object_get_int64(timeField);

	root->mtime = root->atime = time;

	json_object_put(repository);
	return 0;
}

int main(int argc, char** argv)
{
	cachedUid = geteuid();
	cachedGid = getegid();

	root = malloc(sizeof(FilesystemDir));
	if (root == NULL) {
		fprintf(stderr, "malloc(): %s\n", strerror(errno));
		return 1;
	}
	memset(root, 0, sizeof(FilesystemDir));

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	memset(&conf, 0, sizeof(conf));

	fuse_opt_parse(&args, &conf, bucse_opts, bucse_opt_proc);

	int err = 0;
	if (strncmp(conf.repository, "file://", 7) == 0) {
		conf.repositoryRealPath = realpath(conf.repository + 7, NULL);
		destination = &destinationLocal;
		err = destination->init(conf.repositoryRealPath);
	// TODO: implement more destinations
	//} else if (strncmp(conf.repository, "ssh://", 6) == 0) {
	} else {
		conf.repositoryRealPath = realpath(conf.repository, NULL);
		destination = &destinationLocal;
		err = destination->init(conf.repositoryRealPath);
	}

	if (err != 0)
	{
		fprintf(stderr, "destination->init(): %d\n", err);
		recursivelyFreeFilesystem(root);
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 2;
	}
	destination->setCallbackActionAdded(&actionAddedDecrypt);

	// TODO: move json parsing to a separate function
	if (parseRepositoryJsonFile() != 0) {
		fprintf(stderr, "parseRepositoryJsonFile() failed\n");

		destination->shutdown();
		recursivelyFreeFilesystem(root);
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 3;
	}

	if (encryption->needsPassphrase() && conf.passphrase == NULL) {
		int gotPass = 0;

		if (isatty(fileno(stdin)) && isatty(fileno(stdout))) {
			fprintf(stdout, "Passphrase: ");
			fflush(stdout);

			struct termios term;
			tcgetattr(fileno(stdin), &term);

			term.c_lflag &= ~ECHO;
			tcsetattr(fileno(stdin), 0, &term);

			char passwd[1024];
			memset(passwd, 0, 1024);
			fgets(passwd, sizeof(passwd), stdin);

			term.c_lflag |= ECHO;
			tcsetattr(fileno(stdin), 0, &term);
			fprintf(stdout, "\n");

			passwd[1024 - 1] = 0;
			if (strlen(passwd) > 0 && passwd[strlen(passwd)-1] == '\n') {
				passwd[strlen(passwd)-1] = 0;
			}
			if (strlen(passwd) > 0) {
				conf.passphrase = malloc(strlen(passwd) + 1);
				if (conf.passphrase == NULL) {
					fprintf(stderr, "malloc() failed\n");
				} else {
					memcpy(conf.passphrase, passwd, strlen(passwd)+1);
					gotPass = 1;
				}
			}
		}

		if (gotPass == 0) {
			fprintf(stderr, "Encryption needs a passphrase\n");

			destination->shutdown();
			recursivelyFreeFilesystem(root);
			actionsCleanup();
			fuse_opt_free_args(&args);
			confCleanup();
			return 4;
		}
	}

	err = parseRepositoryFile();
	if (err != 0) {
		fprintf(stderr, "parseRepositoryFile() failed\n");
		if (err == 4) {
			fprintf(stderr, "Is the passphrase correct?\n");
		}

		destination->shutdown();
		recursivelyFreeFilesystem(root);
		actionsCleanup();
		fuse_opt_free_args(&args);
		confCleanup();
		return 5;
	}

	int fuse_stat;
	fuse_stat = bucse_fuse_main(args.argc, args.argv, &bucse_oper, sizeof(bucse_oper), NULL);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
	pthread_mutex_lock(&shutdownMutex);
	shutdownTicking = 1;
	pthread_mutex_unlock(&shutdownMutex);

	int ret = pthread_join(tickThread, NULL);
	if (ret != 0)
	{
		fprintf(stderr, "pthread_join: %d\n", ret);
	}
	destination->shutdown();

	// free filesystem
	recursivelyFreeFilesystem(root);

	actionsCleanup();

	fuse_opt_free_args(&args);
	confCleanup();
	return fuse_stat;
}

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <archive.h>
#include <archive_entry.h>

#include "log.h"

#include "tar.h"

int forEveryFileInTar(char* tarBuf, size_t tarSize, int (*actionAddedDecryptOneAction)(char*, char*, size_t))
{
	// A tar file ends with two 512-byte blocks of zeros (the end-of-archive
	// marker). libarchive doesn't require them: a tar file cut exactly between
	// two entries would be read as a complete one, so check them here.
	if (tarSize < 1024) {
		logPrintf(LOG_ERROR, "forEveryFileInTar: tar file too short\n");
		return ARCHIVE_FATAL;
	}
	for (size_t i = tarSize - 1024; i < tarSize; i++) {
		if (tarBuf[i] != 0) {
			logPrintf(LOG_ERROR, "forEveryFileInTar: no end-of-archive marker\n");
			return ARCHIVE_FATAL;
		}
	}

	struct archive* a = archive_read_new();
	struct archive_entry* entry;
	int r;

	// enable tar format reading
	archive_read_support_format_tar(a);

	// open archive from memory buffer
	r = archive_read_open_memory(a, tarBuf, tarSize);
	if (r != ARCHIVE_OK) {
		archive_read_free(a);
		logPrintf(LOG_ERROR, "forEveryFileInTar: archive_read_open_memory failed: %d\n", r);
		return r;
	}

	// in the first pass: count total files to be handled
	int fileCounter = 0;
	while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
		if (archive_entry_filetype(entry) == AE_IFREG) {
			fileCounter++;
		}
	}
	logPrintf(LOG_VERBOSE_DEBUG, "files in tar archive: %d\n", fileCounter);

	// reopen the archive
	archive_read_close(a);
	archive_read_free(a);
	a = archive_read_new();
	archive_read_support_format_tar(a);
	r = archive_read_open_memory(a, tarBuf, tarSize);
	if (r != ARCHIVE_OK) {
		archive_read_free(a);
		logPrintf(LOG_ERROR, "forEveryFileInTar: archive_read_open_memory failed: %d\n", r);
		return r;
	}

	// iterate through entries in the archive
	while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {

		// only process regular files
		if (archive_entry_filetype(entry) == AE_IFREG) {
			// the tar file comes from the untrusted server: an entry
			// can't be larger than the whole tar file
			la_int64_t entrySize = archive_entry_size(entry);
			if (entrySize < 0 || (uint64_t)entrySize > tarSize) {
				logPrintf(LOG_ERROR, "forEveryFileInTar: invalid entry size: %lld\n",
					(long long)entrySize);
				archive_read_free(a);
				return ARCHIVE_FATAL;
			}
			size_t fileSize = (size_t)entrySize;
			// malloc(0) may return NULL
			char* fileBuf = malloc(fileSize > 0 ? fileSize : 1);
			if (!fileBuf) {
				archive_read_free(a);
				return ARCHIVE_FATAL;
			}

			// read file data into buffer
			ssize_t totalRead = 0;
			while (totalRead < (ssize_t)fileSize) {
				const void* buf;
				size_t size;
				la_int64_t offset;
				r = archive_read_data_block(a, &buf, &size, &offset);
				if (r == ARCHIVE_EOF)
					break;
				if (r != ARCHIVE_OK) {
					free(fileBuf);
					archive_read_free(a);
					return r;
				}
				if (offset < 0 || (uint64_t)offset > fileSize
						|| size > fileSize - (size_t)offset) {
					logPrintf(LOG_ERROR, "forEveryFileInTar: entry data outside of the entry size\n");
					free(fileBuf);
					archive_read_free(a);
					return ARCHIVE_FATAL;
				}
				memcpy(fileBuf + offset, buf, size);
				totalRead += size;
			}

			// decrement file counter to know if this is the last one
			fileCounter--;
			// an entry that is rejected is ignored, unless reading
			// it again may work (see TAR_ENTRY_RETRY)
			int result = actionAddedDecryptOneAction(
				(char*)archive_entry_pathname(entry),
				fileBuf,
				fileSize);

			free(fileBuf);
			if (result == TAR_ENTRY_RETRY) {
				logPrintf(LOG_ERROR, "forEveryFileInTar: entry %s can't be handled now, the tar file will be read again\n",
					archive_entry_pathname(entry));
				archive_read_free(a);
				return ARCHIVE_FATAL;
			}
		} else {
			// Skip non-regular files by reading data to advance
			archive_read_data_skip(a);
		}
	}

	archive_read_close(a);
	archive_read_free(a);

	// Return success or last error code (usually ARCHIVE_EOF)
	return (r == ARCHIVE_EOF) ? ARCHIVE_OK : r;
}


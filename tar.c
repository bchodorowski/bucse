#include <stdlib.h>
#include <string.h>

#include <archive.h>
#include <archive_entry.h>

#include "log.h"

#include "tar.h"

int forEveryFileInTar(char* tarBuf, size_t tarSize, int moreInThisBatch, void (*actionAddedDecryptOneAction)(char*, char*, size_t, int))
{
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
	logPrintf(LOG_VERBOSE_DEBUG, "files in tar archive:: %d\n", fileCounter);

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
			size_t fileSize = archive_entry_size(entry);
			char* fileBuf = malloc(fileSize);
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
				memcpy(fileBuf + offset, buf, size);
				totalRead += size;
			}

			// decrement file counter to know if this is the last one
			fileCounter--;
			actionAddedDecryptOneAction(
				(char*)archive_entry_pathname(entry),
				fileBuf,
				fileSize,
				moreInThisBatch ? moreInThisBatch : fileCounter);

			free(fileBuf);
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


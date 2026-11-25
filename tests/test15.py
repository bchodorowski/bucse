#!/bin/python3

# A shrinking truncate whose flush fails (storage/ is not writable, so the
# shortened last block can't be stored) must be kept: the next flush, once
# storage/ is writable again, truncates to the same size, not to 0. When the
# tests run as root, storage/ stays writable and the flush doesn't fail.

import bucseTests
import errno
import os


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

# 10000 bytes: 2048-byte blocks, so at 3000 bytes the last block is partial
# and has to be rewritten
FILE_SIZE = 10000
TRUNC_SIZE = 3000

path = "test_%d/file" % bucseTests.pid
data = os.urandom(FILE_SIZE)
with open(path, "wb") as f:
    f.write(data)

fd = os.open(path, os.O_RDWR)
os.ftruncate(fd, TRUNC_SIZE)

# a read flushes the file first; that flush fails
bucseTests.chmodRepoFile("storage", 0o555)
try:
    os.pread(fd, FILE_SIZE, 0)
    print("the flush didn't fail (running as root?)")
except OSError as e:
    assert e.errno == errno.EIO, e
finally:
    bucseTests.chmodRepoFile("storage", 0o755)

# the retried flush succeeds and truncates to TRUNC_SIZE
assert os.pread(fd, FILE_SIZE, 0) == data[:TRUNC_SIZE]
assert os.fstat(fd).st_size == TRUNC_SIZE
os.close(fd)

bucseTests.verifyFailOnError(True, allowedErrors = [
    "writeWholeFile: fopen(): Permission denied",
    "destLocalPutStorageFile: writeWholeFile() failed",
    "destSshPutStorageFile: sftp_open()",
    "flushFile: putStorageFile failed",
])

# the same after a remount (verifyFailOnError() remounts)
with open(path, "rb") as f:
    assert f.read() == data[:TRUNC_SIZE]

bucseTests.testCleanup()

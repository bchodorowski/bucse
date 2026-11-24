#!/bin/python3

# Reads copy only the requested bytes from a (cached) block. Every read used
# to copy the whole block, up to 128 MB, for a FUSE read of at most 128 KB.
# 1. Reads at many offsets, also across block boundaries and past the end of
#    the file, return the right data.
# 2. A block shorter than the file says is rejected on every read that needs
#    the missing bytes, also when the block is already in the cache: a cache
#    hit used to skip the check and return stale bytes of a buffer.
# The reads use O_DIRECT, so they reach bucse-mount with the exact offset and
# size, not rounded to pages and grouped by the kernel's readahead.

import bucseTests
import errno
import hashlib
import json
import mmap
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()


def mountPath(path):
    return "test_%d/%s" % (bucseTests.pid, path)


def directRead(path, offset, size):
    fd = os.open(mountPath(path), os.O_RDONLY | os.O_DIRECT)
    try:
        # O_DIRECT needs an aligned buffer, mmap gives a page-aligned one
        buf = mmap.mmap(-1, max(size, 1))
        n = os.preadv(fd, [buf], offset)
        return buf[:n]
    finally:
        os.close(fd)


# 1. A file of 4 blocks of 256 KB and a partial one (its block size is chosen
# from its size when it's flushed)
BLOCK_SIZE = 256 * 1024
data = os.urandom(4 * BLOCK_SIZE + 1234)
with open(mountPath("big"), "wb") as f:
    f.write(data)

# the data must come from the storage blocks, not from the pending writes
bucseTests.remount()

reads = [(0, 1), (0, 4096), (BLOCK_SIZE - 1, 2), (BLOCK_SIZE - 100, 300),
         (BLOCK_SIZE, BLOCK_SIZE), (3 * BLOCK_SIZE + 5, 128 * 1024),
         (BLOCK_SIZE + 17, 2 * BLOCK_SIZE + 3), (len(data) - 10, 100),
         (len(data), 10), (len(data) + 1000, 10), (0, len(data) + 1000)]
for offset, size in reads:
    content = directRead("big", offset, size)
    assert content == data[offset:offset + size], \
        "wrong data read at offset %d, size %d" % (offset, size)

with open(mountPath("big"), "rb") as f:
    assert f.read() == data, "wrong data read with buffered reads"

# 2. A file of 2 blocks of 512 bytes whose second block has only 100 bytes
SHORT_BLOCK_SIZE = 512
blocks = [os.urandom(SHORT_BLOCK_SIZE), os.urandom(100)]
blockNames = []
for blockData in blocks:
    blockName = hashlib.sha1(blockData).hexdigest()
    bucseTests.writeRepoFile("storage/%s" % blockName, bucseTests.encryptForRepo(blockData))
    blockNames.append(blockName)
action = json.dumps([{
    "time": int(time.time() * 1000000),
    "action": "addFile",
    "path": "short",
    "content": blockNames,
    "size": 2 * SHORT_BLOCK_SIZE,
    "blockSize": SHORT_BLOCK_SIZE,
}]).encode("utf-8")
bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(action).hexdigest(),
    bucseTests.encryptForRepo(action))
bucseTests.waitForFiles(["short"])

# the bytes the short block has can be read, and the block is now cached
assert directRead("short", SHORT_BLOCK_SIZE, 50) == blocks[1][:50]
assert directRead("short", SHORT_BLOCK_SIZE, 100) == blocks[1]
# the bytes it doesn't have can't, also from the cached block
for offset, size in [(SHORT_BLOCK_SIZE + 50, 100), (SHORT_BLOCK_SIZE + 200, 10),
                     (0, 2 * SHORT_BLOCK_SIZE)]:
    try:
        content = directRead("short", offset, size)
        assert False, "read at offset %d, size %d succeeded: %d bytes" % \
            (offset, size, len(content))
    except OSError as e:
        assert e.errno == errno.EIO, "offset %d, size %d: %s" % (offset, size, e)
# the first block is fine
assert directRead("short", 0, SHORT_BLOCK_SIZE) == blocks[0]

bucseTests.verifyFailOnError(True, allowedErrors = [
    "decryptBlock: expected decrypted block size at least",
])
bucseTests.testCleanup()

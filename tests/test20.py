#!/bin/python3

# A shrinking truncate followed by a write past the new end, flushed
# together: the bytes between the new end and the write must be zeroes (the
# old bytes used to be kept, e.g. 1000 x "A", ftruncate(10), pwrite("X", 20)
# read back as 20 x "A" + "X"). Covered cases:
# - the cut and the write in the same block,
# - blocks between them that the write doesn't touch,
# - a write so far that the blocks get resized,
# - O_TRUNC instead of ftruncate,
# - a cut on a block boundary.
# The results must be the same after a remount, i.e. in the repository.

import bucseTests
import os


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mnt = "test_%d" % bucseTests.pid


def path(name):
    return "%s/%s" % (mnt, name)


def readFile(name):
    with open(path(name), "rb") as f:
        return f.read()


# (name, old size, truncate to (None: O_TRUNC), write offset)
cases = [
    ("sameBlock", 1000, 10, 20),       # block size 512
    ("untouchedBlocks", 4096, 10, 3000),  # block size 1024
    ("resized", 1000, 10, 512 * 40),   # more than 32 blocks of 512
    ("oTrunc", 1000, None, 20),
    ("blockBoundary", 4096, 1024, 2500),
]

expected = {}
for name, oldSize, truncSize, offset in cases:
    with open(path(name), "wb") as f:
        f.write(b"A" * oldSize)

for name, oldSize, truncSize, offset in cases:
    if truncSize is None:
        fd = os.open(path(name), os.O_WRONLY | os.O_TRUNC)
        truncSize = 0
    else:
        fd = os.open(path(name), os.O_WRONLY)
        os.ftruncate(fd, truncSize)
    os.pwrite(fd, b"X", offset)
    os.close(fd)
    expected[name] = b"A" * truncSize + b"\0" * (offset - truncSize) + b"X"


def verify():
    for name, data in expected.items():
        actual = readFile(name)
        assert actual == data, "%s: %r, expected %r" % (name, actual[:40], data[:40])


verify()

# remounts, everything is read from the repository again
bucseTests.verifyFailOnError()
verify()

bucseTests.testCleanup()

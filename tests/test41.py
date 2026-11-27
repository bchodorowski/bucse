#!/bin/python3

# Small sequential writes (dd bs=512) are merged into one pending write, so
# they don't reach the pending writes count limit (10240) and the file isn't
# flushed every 5 MB. Every such flush rewrote the partial block at the end:
# copying a 5 GB file this way stored about 60 GB. A stat doesn't flush the
# file either (under valgrind, the kernel itself stats it during the writes). Writes that aren't
# sequential (going back, overwriting, then going on) still give the right
# content.

import bucseTests
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mount = "test_%d" % bucseTests.pid
mirror = "test_%d_mirror" % bucseTests.pid

# 1. more 512-byte writes than the count limit: written by one flush, when
# the file is closed, so only one action file (not with dd: it closes a
# duplicate of the descriptor first, which flushes the empty file)
writesCount = 11000
fileName = bucseTests.makeRandomTmpFile(writesCount * 512, False)
with open("tmp/%s" % fileName, "rb") as f:
    data = f.read()

actionsBefore = len(bucseTests.listRepoDir("actions"))
lastMtime = 0
for d in [mount, mirror]:
    fd = os.open("%s/seq" % d, os.O_CREAT | os.O_WRONLY)
    for offset in range(0, len(data), 512):
        os.write(fd, data[offset:offset + 512])
        # a stat while the file is being written doesn't flush it: it shows
        # the size the pending writes extend it to, and the time of the last
        # write as the mtime
        if d == mount and offset % (2000 * 512) == 0:
            st = os.stat("%s/seq" % d)
            assert st.st_size == offset + 512, (st.st_size, offset)
            assert lastMtime < st.st_mtime_ns <= time.time_ns(), (lastMtime, st.st_mtime_ns)
            lastMtime = st.st_mtime_ns
            assert len(bucseTests.listRepoDir("actions")) == actionsBefore
    os.close(fd)
actionsAfter = len(bucseTests.listRepoDir("actions"))
assert actionsAfter - actionsBefore == 1, (actionsBefore, actionsAfter)

# 2. sequential writes mixed with others: a write back into what was just
# written, then on from there (merged with it), then on from the end
for d in [mount, mirror]:
    fd = os.open("%s/mixed" % d, os.O_CREAT | os.O_WRONLY)
    for offset in range(0, 4096, 512):
        os.pwrite(fd, data[offset:offset + 512], offset)
    os.pwrite(fd, b"x" * 100, 1000)
    os.pwrite(fd, b"y" * 100, 1100)
    for offset in range(4096, 8192, 512):
        os.pwrite(fd, data[offset:offset + 512], offset)
    # a gap: the bytes in between read as zeroes
    os.pwrite(fd, b"z" * 512, 10000)
    os.pwrite(fd, b"w" * 512, 10512)
    os.close(fd)

bucseTests.verifyWithMirror()

bucseTests.verifyFailOnError()

bucseTests.testCleanup()

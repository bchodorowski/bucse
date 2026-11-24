#!/bin/python3

# A file that is being written is flushed once its pending writes add up to
# 128 MB, not only after 10240 writes: writing a big file used to keep up to
# 10240 writes (about 1.3 GB with 128 KB writes) in memory, which made
# bucse-mount run out of memory on a machine with little RAM.

import bucseTests
import os


bucseTests.parseArgs()

bucseTests.mountDirs()

MB = 1024 * 1024
size = 129 * MB
path = "test_%d/big" % bucseTests.pid

data = os.urandom(size)
actionsBefore = len(bucseTests.listRepoDir("actions"))

fd = os.open(path, os.O_CREAT | os.O_WRONLY)
# 1 MB writes: FUSE splits them into at most 128 KB ones, about 1000 of
# them, far fewer than 10240
for offset in range(0, size, MB):
    os.write(fd, data[offset:offset + MB])

# the file is still open, so only the size limit can have flushed it
actionsAfter = len(bucseTests.listRepoDir("actions"))
assert actionsAfter > actionsBefore, "no action file written before close"

os.close(fd)

def verify():
    with open(path, "rb") as f:
        assert f.read() == data

verify()
bucseTests.remount()
verify()

bucseTests.testCleanup()

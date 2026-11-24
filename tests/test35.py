#!/bin/python3

# A truncate by path (truncate(2), no open handle) is flushed right away: it
# writes its action before the call returns. It used to stay pending until the
# file was released through another handle, or the unmount, so a crash lost
# it. A truncate of an open file (ftruncate), an open with O_TRUNC, or a create
# without writes is flushed when the file is closed, before close() returns
# (they used to be left to the release, which comes after it).

import bucseTests
import os


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mnt = "test_%d" % bucseTests.pid
mirror = "test_%d_mirror" % bucseTests.pid


def writeFile(name, data):
    for d in [mnt, mirror]:
        with open("%s/%s" % (d, name), "wb") as f:
            f.write(data)


def truncateByPath(name, size):
    for d in [mnt, mirror]:
        os.truncate("%s/%s" % (d, name), size)


def actionsCount():
    return len(bucseTests.listRepoDir("actions"))


writeFile("shrunk", os.urandom(5000))
writeFile("extended", os.urandom(700))

# by path: an action is written for each truncate before it returns
count = actionsCount()
truncateByPath("shrunk", 1234)
assert actionsCount() == count + 1, "the shrinking truncate wasn't flushed"
truncateByPath("extended", 100000)
assert actionsCount() == count + 2, "the extending truncate wasn't flushed"
# the same size: nothing to write
truncateByPath("extended", 100000)
assert actionsCount() == count + 2, "a truncate to the same size was written"

# through an open handle: written when the file is closed, before close()
# returns (it used to be left to the release, which the kernel sends after
# close() returns)
# (kept open by a FileHolder, so that ssh subprocesses don't flush it)
holder = bucseTests.FileHolder()
fd = holder.open("%s/shrunk" % mnt, os.O_WRONLY)
holder.ftruncate(fd, 10)
os.truncate("%s/shrunk" % mirror, 10)
assert actionsCount() == count + 2, "ftruncate was flushed before the close"
holder.close(fd)
assert actionsCount() == count + 3, "ftruncate wasn't flushed by the close"

# an open with O_TRUNC, closed without writes
writeFile("truncatedOnOpen", os.urandom(3000))
count = actionsCount()
for d in [mnt, mirror]:
    os.close(os.open("%s/truncatedOnOpen" % d, os.O_WRONLY | os.O_TRUNC))
assert actionsCount() == count + 1, "O_TRUNC wasn't flushed by the close"

# a file created and closed without writes
for d in [mnt, mirror]:
    os.close(os.open("%s/created" % d, os.O_WRONLY | os.O_CREAT, 0o644))
assert actionsCount() == count + 2, "the create wasn't flushed by the close"

bucseTests.verifyWithMirror()
bucseTests.verifyFailOnError()
bucseTests.testCleanup()

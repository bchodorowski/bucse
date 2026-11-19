#!/bin/python3

# Read-only mode (-R) must not change the repository:
# - open(O_RDONLY | O_TRUNC) fails with EROFS (it used to succeed, empty the
#   file and write an editFile action when the file was closed),
# - open(O_RDONLY | O_CREAT) of a file that doesn't exist fails with EROFS,
#   of an existing one it succeeds,
# - reading still works.
# After a remount in normal mode, the files must be unchanged.

import bucseTests
import errno
import os


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mnt = "test_%d" % bucseTests.pid
content = b"hello, read-only world\n" * 100


def path(name):
    return "%s/%s" % (mnt, name)


def readFile(name):
    with open(path(name), "rb") as f:
        return f.read()


def expectError(flags, name, expectedErrno):
    try:
        fd = os.open(path(name), flags)
    except OSError as e:
        assert e.errno == expectedErrno, "%s: errno %d, expected %d" % (name, e.errno, expectedErrno)
        return
    os.close(fd)
    raise Exception("%s: open with flags %o succeeded in read-only mode" % (name, flags))


with open(path("a"), "wb") as f:
    f.write(content)

bucseTests.verifyFailOnError()

bucseTests.remount(["-R"])

expectError(os.O_RDONLY | os.O_TRUNC, "a", errno.EROFS)
expectError(os.O_RDONLY | os.O_CREAT, "new", errno.EROFS)
fd = os.open(path("a"), os.O_RDONLY | os.O_CREAT)
os.close(fd)
assert readFile("a") == content
assert not os.path.exists(path("new"))

# normal mode again, everything is read from the repository
bucseTests.remount()

assert readFile("a") == content
assert not os.path.exists(path("new"))

bucseTests.testCleanup()

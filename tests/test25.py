#!/bin/python3

# Renaming a directory onto an existing empty directory replaces it (it used
# to fail with EEXIST).

import bucseTests
import errno
import os


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mount = "test_%d" % bucseTests.pid

# a directory with contents onto an empty one
os.makedirs("%s/src/sub" % mount)
with open("%s/src/f" % mount, "wb") as f:
    f.write(b"content")
os.mkdir("%s/dst" % mount)
os.rename("%s/src" % mount, "%s/dst" % mount)

# an empty directory onto an empty one
os.mkdir("%s/emptySrc" % mount)
os.mkdir("%s/emptyDst" % mount)
os.rename("%s/emptySrc" % mount, "%s/emptyDst" % mount)

# a directory onto a directory that isn't empty still fails
os.mkdir("%s/a" % mount)
os.makedirs("%s/b/keep" % mount)
try:
    os.rename("%s/a" % mount, "%s/b" % mount)
    raise Exception("rename onto a non-empty directory succeeded")
except OSError as e:
    assert e.errno == errno.ENOTEMPTY, e

def check():
    assert sorted(os.listdir(mount)) == ["a", "b", "dst", "emptyDst"], os.listdir(mount)
    assert sorted(os.listdir("%s/dst" % mount)) == ["f", "sub"]
    with open("%s/dst/f" % mount, "rb") as f:
        assert f.read() == b"content"
    assert os.listdir("%s/emptyDst" % mount) == []
    assert os.listdir("%s/b" % mount) == ["keep"]

check()

# also after a remount
bucseTests.verifyFailOnError(allowedErrors = [
    "bucse_rename: dir not empty",
])
check()

bucseTests.testCleanup()

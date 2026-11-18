#!/bin/python3

# Actions of another client must not drop local changes that haven't been
# flushed yet (they used to be dropped, and RemoveDirectory freed a directory
# together with its contents):
# - editFile: the local changes are applied on top of the new content,
# - removeFile: the file is kept and added again when it is flushed,
# - removeDirectory of a directory with an unflushed new file: the directories
#   are kept and added again before the file,
# - removeDirectory of a directory with a file in the repository (the other
#   client hadn't seen it yet): the directory is kept on every client,
# - addFile of a file created here but not flushed yet, with nothing written:
#   the added file is taken.
# The results must be the same after a remount, i.e. in the repository.
#
# The local changes are truncates and creates: they stay unflushed until the
# file is closed, while written data is flushed by getattr already.

import bucseTests
import hashlib
import json
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mnt = "test_%d" % bucseTests.pid


def path(name):
    return "%s/%s" % (mnt, name)


def writeFile(name, data):
    with open(path(name), "wb") as f:
        f.write(data)


def readFile(name):
    with open(path(name), "rb") as f:
        return f.read()


# an action of another client, written straight into the repository
def addRemoteAction(name, actionType, actionTime, content = [], size = 0, blockSize = 0):
    action = [{
        "time": actionTime,
        "action": actionType,
        "path": name,
        "content": content,
        "size": size,
        "blockSize": blockSize,
    }]
    data = json.dumps(action).encode("utf-8")
    bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
        bucseTests.encryptForRepo(data))


# a block of another client
remoteData = b"S" * 1000
remoteBlock = hashlib.sha1(b"test19 block %d" % bucseTests.pid).hexdigest()
bucseTests.writeRepoFile("storage/%s" % remoteBlock, bucseTests.encryptForRepo(remoteData))

# files and directories in the repository
writeFile("edited", b"A" * 1000)
writeFile("removed", b"C" * 100)
os.mkdir(path("dir"))
os.mkdir(path("dir/sub"))
os.mkdir(path("dirWithFile"))
writeFile("dirWithFile/file", b"G" * 10)

# local changes that aren't flushed while the files are open
fdEdited = os.open(path("edited"), os.O_WRONLY)
os.ftruncate(fdEdited, 10)
fdRemoved = os.open(path("removed"), os.O_WRONLY)
os.ftruncate(fdRemoved, 5)
fdNew = os.open(path("dir/sub/new"), os.O_WRONLY | os.O_CREAT, 0o644)
fdAdded = os.open(path("added"), os.O_WRONLY | os.O_CREAT, 0o644)

# actions of another client
t = int(time.time() * 1000000)
addRemoteAction("edited", "editFile", t, [remoteBlock], len(remoteData), 1024)
addRemoteAction("removed", "removeFile", t + 1)
addRemoteAction("dir/sub", "removeDirectory", t + 2)
addRemoteAction("dir", "removeDirectory", t + 3)
addRemoteAction("dirWithFile", "removeDirectory", t + 4)
addRemoteAction("added", "addFile", t + 5, [remoteBlock], len(remoteData), 1024)
bucseTests.waitForActionsHandled()

# closing flushes the files
for fd in [fdEdited, fdRemoved, fdNew, fdAdded]:
    os.close(fd)

expected = {
    "edited": b"S" * 10,
    "removed": b"C" * 5,
    "dir/sub/new": b"",
    "dirWithFile/file": b"G" * 10,
}


def verify():
    for name, data in expected.items():
        actual = readFile(name)
        assert actual == data, "%s: %r, expected %r" % (name, actual[:40], data[:40])


verify()

# remounts, everything is read from the repository again
bucseTests.verifyFailOnError()
# checked only now: until the kernel's cached attributes (from the create)
# expire, it still sees an empty file
expected["added"] = remoteData
verify()

bucseTests.testCleanup()

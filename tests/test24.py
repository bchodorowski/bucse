#!/bin/python3

# An addFile of a path where a directory exists, or an addDirectory where a
# file exists:
# - if the existing entry is in the repository, the action is rejected
#   (every client applies actions in the same order, so they all reject it);
# - if it is local and not in the repository yet (an unflushed file, or a
#   directory that holds only local data), it is renamed to "name.conflict",
#   so its data is not lost.

import bucseTests
import json
import hashlib
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mount = "test_%d" % bucseTests.pid

def action(actionType, path):
    return {"time": int(time.time() * 1000000), "action": actionType,
            "path": path, "content": [], "size": 0, "blockSize": 0}

def writeAction(actionType, path):
    data = json.dumps([action(actionType, path)]).encode("utf-8")
    bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
        bucseTests.encryptForRepo(data))

def closeIgnoringError(fd):
    # flush and release are path based, the old path now leads to the other
    # client's entry
    try:
        holder.close(fd)
    except OSError:
        pass

# 1. Entries in the repository: the actions are rejected
os.mkdir("%s/dir" % mount)
with open("%s/file" % mount, "wb") as f:
    f.write(b"file")
writeAction("addFile", "dir")
writeAction("addDirectory", "file")
bucseTests.waitForActionsHandled()

# The local files below are created but nothing is written to them before the
# other client's action is applied (getattr used to flush a file with pending
# writes, and the kernel may call it at any time, which would have put the
# file into the repository before the action). They are kept open by a
# FileHolder, so that the test's subprocesses (openssl, ssh) don't flush them.
holder = bucseTests.FileHolder()

# 2. A local unflushed file, then another client adds a directory with its
# name
fdFile = holder.open("%s/x" % mount, os.O_WRONLY | os.O_CREAT, 0o644)
writeAction("addDirectory", "x")

# 3. A directory kept after another client removed it (it holds an
# unflushed file), then another client adds a file with its name
os.mkdir("%s/d" % mount)
fdInDir = holder.open("%s/d/f" % mount, os.O_WRONLY | os.O_CREAT, 0o644)
writeAction("removeDirectory", "d")
bucseTests.waitForActionsHandled()
writeAction("addFile", "d")
bucseTests.waitForFiles(["x.conflict", "d.conflict/f"])
closeIgnoringError(fdFile)
closeIgnoringError(fdInDir)

# the renamed entries can be used under their new names
with open("%s/x.conflict" % mount, "wb") as f:
    f.write(b"unflushed file")
with open("%s/d.conflict/f" % mount, "wb") as f:
    f.write(b"unflushed in dir")

def check():
    assert os.path.isdir("%s/dir" % mount)
    with open("%s/file" % mount, "rb") as f:
        assert f.read() == b"file"
    assert os.path.isdir("%s/x" % mount)
    with open("%s/x.conflict" % mount, "rb") as f:
        assert f.read() == b"unflushed file"
    assert os.path.isfile("%s/d" % mount)
    with open("%s/d.conflict/f" % mount, "rb") as f:
        assert f.read() == b"unflushed in dir"
    entries = os.listdir(mount)
    for n in ["dir", "file", "x", "x.conflict", "d", "d.conflict"]:
        assert entries.count(n) == 1, entries

check()

# after a remount everything is read from the repository
bucseTests.verifyFailOnError(allowedErrors = [
    "doAction: a directory with that name already exists",
    "doAction: a file with that name already exists",
    # closing d/f: d is a file now
    "bucse_flush: path not found",
    "bucse_release: path not found",
])
check()

bucseTests.testCleanup()

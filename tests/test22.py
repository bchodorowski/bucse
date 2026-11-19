#!/bin/python3

# Actions with the same time (a rename writes two of them) must be
# deduplicated whatever order they arrive in. Actions used to be sorted by
# time only, while deduplication assumed identical actions to be adjacent and
# both arrays to be sorted by time, type and path. So a duplicate could be
# missed and applied a second time, e.g. an addDirectory creating the
# directory twice.

import bucseTests
import json
import hashlib
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mount = "test_%d" % bucseTests.pid

def addDirectory(path, t):
    return {"time": t, "action": "addDirectory", "path": path,
            "content": [], "size": 0, "blockSize": 0}

def encryptedActionFile(actions):
    data = json.dumps(actions).encode("utf-8")
    return hashlib.sha1(data).hexdigest(), bucseTests.encryptForRepo(data)

# Two actions with the same time in one action file...
t = int(time.time() * 1000000)
name, data = encryptedActionFile([addDirectory("d1", t), addDirectory("d2", t)])
bucseTests.writeRepoFile("actions/%s" % name, data)
bucseTests.waitForFiles(["d1", "d2"])

# ... and again, in the other order, packed into a tar file (e.g. a packing
# tool, or a retry of a tar file read partially)
name, data = encryptedActionFile([addDirectory("d2", t), addDirectory("d1", t)])
bucseTests.writeRepoFile("actions/dup.tar", bucseTests.makeActionsTar([(name, data)]))
bucseTests.waitForActionsHandled()

entries = os.listdir(mount)
assert entries.count("d1") == 1 and entries.count("d2") == 1, entries

# A rename onto an existing file writes editFile (destination) and removeFile
# (source) with the same time; then all the action files are packed into a
# tar file
with open("%s/a" % mount, "wb") as f:
    f.write(b"source")
with open("%s/b" % mount, "wb") as f:
    f.write(b"destination")
os.rename("%s/a" % mount, "%s/b" % mount)

actionNames = [n for n in bucseTests.listRepoDir("actions") if not n.endswith(".tar")]
bucseTests.writeRepoFile("actions/all.tar", bucseTests.makeActionsTar(
    [(n, bucseTests.readRepoFile("actions/%s" % n)) for n in sorted(actionNames)]))
bucseTests.waitForActionsHandled()

def check():
    entries = os.listdir(mount)
    for n in ["d1", "d2", "b"]:
        assert entries.count(n) == 1, entries
    assert "a" not in entries, entries
    with open("%s/b" % mount, "rb") as f:
        assert f.read() == b"source"

check()

# no errors (a duplicate removeFile fails with "file not found"), and after a
# remount, which reads every action file and the tar files together
bucseTests.verifyFailOnError()
check()

bucseTests.testCleanup()

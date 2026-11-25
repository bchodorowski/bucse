#!/bin/python3

# An addDirectory of a directory that already exists must not add a second
# one: two clients can create the same directory. An out-of-order action
# makes the actions after it be undone and applied again.

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

def writeAction(action):
    data = json.dumps([action]).encode("utf-8")
    bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
        bucseTests.encryptForRepo(data))

t = int(time.time() * 1000000)

# 1. Two clients create the same directory
writeAction(addDirectory("both", t))
writeAction(addDirectory("both", t + 1))

# 2. An action older than "late" arrives after it: "late" is applied again
writeAction(addDirectory("late", t + 3))
bucseTests.waitForActionsHandled()
writeAction(addDirectory("early", t + 2))
bucseTests.waitForFiles(["both", "late", "early"])
bucseTests.waitForActionsHandled()

# 3. A directory with contents, created again by another client
os.mkdir("%s/full" % mount)
with open("%s/full/file" % mount, "wb") as f:
    f.write(b"content")
writeAction(addDirectory("full", int(time.time() * 1000000)))
bucseTests.waitForActionsHandled()

def check():
    entries = os.listdir(mount)
    for n in ["both", "late", "early", "full"]:
        assert entries.count(n) == 1, entries
    assert os.listdir("%s/full" % mount) == ["file"]
    with open("%s/full/file" % mount, "rb") as f:
        assert f.read() == b"content"

check()

# also after a remount, which reads every action file together
bucseTests.verifyFailOnError()
check()

bucseTests.testCleanup()

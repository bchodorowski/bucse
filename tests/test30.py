#!/bin/python3

# A new local action is ordered after every action already applied, even if
# this client's clock is behind (a Lamport clock). Another client, whose clock
# is an hour ahead, adds a file, an empty file and a directory; this client
# then removes the file and the directory and writes into the empty file. Its
# actions used to get earlier times than the ones they follow, so after a
# remount (and on every other client) they were applied first: the removed
# file and directory came back and the written file was empty.

import bucseTests
import json
import os
import time
import hashlib


bucseTests.parseArgs()

bucseTests.failOnError = True

bucseTests.mountDirs()

mountDir = "test_%d" % bucseTests.pid

# One action file, so bucse-mount reads all three actions at once. Markers
# (waitForActionsHandled()) would get earlier times than these actions, and
# out-of-order actions can't be undone yet, so only waitForFiles() is used
# after them.
futureTime = int(time.time() * 1000000) + 3600 * 1000000
actions = []
for i, (path, actionType) in enumerate([("removed", "addFile"),
                                        ("edited", "addFile"),
                                        ("removedDir", "addDirectory")]):
    actions.append({
        "time": futureTime + i,
        "action": actionType,
        "path": path,
        "content": [],
        "size": 0,
        "blockSize": 0,
    })
data = json.dumps(actions).encode("utf-8")
bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
                         bucseTests.encryptForRepo(data))
bucseTests.waitForFiles(["removed", "edited", "removedDir"])

os.remove("%s/removed" % mountDir)
os.rmdir("%s/removedDir" % mountDir)
with open("%s/edited" % mountDir, "wb") as f:
    f.write(b"new content")

def check():
    entries = sorted(os.listdir(mountDir))
    assert entries == ["edited"], entries
    with open("%s/edited" % mountDir, "rb") as f:
        content = f.read()
    assert content == b"new content", content
    # the edit is ordered after the addFile it follows
    mtime = os.stat("%s/edited" % mountDir).st_mtime_ns // 1000
    assert mtime > futureTime, (mtime, futureTime)

check()

bucseTests.remount()
check()

bucseTests.verifyFailOnError()
bucseTests.testCleanup()

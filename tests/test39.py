#!/bin/python3

# Out-of-order actions: an action file that arrives after actions with later
# times makes bucse-mount undo those actions and apply them again after it, so
# the result is the same as on a remount, which applies every action in the
# order of their times. Unflushed local changes (a file created, or truncated,
# with an open descriptor) are kept.
#
# The actions written here get times an hour in the past, so they are older
# than the markers of waitForActionsHandled() and the local actions; each
# "late" one makes everything after it be undone and applied again.

import bucseTests
import hashlib
import json
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mount = "test_%d" % bucseTests.pid
holder = bucseTests.FileHolder()

base = int(time.time() * 1000000) - 3600 * 1000000

def writeAction(actionType, path, offset):
    data = json.dumps([{"time": base + offset, "action": actionType, "path": path,
                        "content": [], "size": 0, "blockSize": 0}]).encode("utf-8")
    bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
        bucseTests.encryptForRepo(data))

def late(name, offset):
    writeAction("addDirectory", name, offset)
    bucseTests.waitForFiles([name])
    bucseTests.waitForActionsHandled()

# 1. An added file is not added twice ("file already exists")
writeAction("addFile", "x", 20)
bucseTests.waitForActionsHandled()
late("late1", 10)

# 2. A removeFile older than the addFile it arrives after: the file stays
writeAction("addFile", "z", 31)
bucseTests.waitForActionsHandled()
writeAction("removeFile", "z", 30)
bucseTests.waitForActionsHandled()

# 3. An addDirectory older than the removeDirectory it arrives after: the
# directory is gone
writeAction("removeDirectory", "d", 41)
bucseTests.waitForActionsHandled()
writeAction("addDirectory", "d", 40)
bucseTests.waitForActionsHandled()

# 4. An editFile older than the one before it: the newer mtime stays
writeAction("addFile", "e", 50)
writeAction("editFile", "e", 55)
bucseTests.waitForActionsHandled()
writeAction("editFile", "e", 53)
bucseTests.waitForActionsHandled()

# 5. Files created locally (not flushed yet, with and without data) after
# another client removed them: put aside while the removal is undone
for name in ["r", "r2"]:
    writeAction("addFile", name, 60)
bucseTests.waitForActionsHandled()
for name in ["r", "r2"]:
    writeAction("removeFile", name, 62)
bucseTests.waitForActionsHandled()
fdR = holder.open("%s/r" % mount, os.O_CREAT | os.O_WRONLY)
holder.write(fdR, b"abc")
fdR2 = holder.open("%s/r2" % mount, os.O_CREAT | os.O_WRONLY)
late("late5", 61)

# 6. A directory removed by another client, kept for a file created in it
writeAction("addDirectory", "kd", 70)
bucseTests.waitForActionsHandled()
fdKd = holder.open("%s/kd/f" % mount, os.O_CREAT | os.O_WRONLY)
holder.write(fdKd, b"kept")
writeAction("removeDirectory", "kd", 72)
bucseTests.waitForActionsHandled()
late("late6", 71)

# 7. The same, but the file was flushed (a truncate flushes a created file
# first) before the older removeDirectory arrived: its addFile is rejected,
# as on every other client, and the file is kept as a local one, so its flush
# adds the directory and the file again
writeAction("addDirectory", "kd2", 75)
bucseTests.waitForActionsHandled()
fdKd2 = holder.open("%s/kd2/f" % mount, os.O_CREAT | os.O_WRONLY)
holder.ftruncate(fdKd2, 3)
writeAction("removeDirectory", "kd2", 76)
bucseTests.waitForActionsHandled()

# 8. A file created locally, then added by another client
fdQ = holder.open("%s/q" % mount, os.O_CREAT | os.O_WRONLY)
writeAction("addFile", "q", 80)
bucseTests.waitForActionsHandled()
late("late8", 79)

# 9. A file edited by another client while it has a pending truncate
writeAction("addFile", "w", 90)
bucseTests.waitForActionsHandled()
fdW = holder.open("%s/w" % mount, os.O_WRONLY)
holder.ftruncate(fdW, 7)
writeAction("editFile", "w", 95)
bucseTests.waitForActionsHandled()
late("late9", 93)

# (listdir only, not stat: it was written when a stat flushed the files with
# pending writes)
entries = os.listdir(mount)
for n in ["x", "z", "e", "r", "r2", "kd", "kd2", "q", "w"]:
    assert entries.count(n) == 1, entries
assert "d" not in entries, entries
assert os.listdir("%s/kd" % mount) == ["f"]
assert os.listdir("%s/kd2" % mount) == ["f"]

for fd in [fdR, fdR2, fdKd, fdKd2, fdQ, fdW]:
    holder.close(fd)
holder.stop()

def check():
    entries = sorted(n for n in os.listdir(mount) if not n.startswith("marker_"))
    assert entries == sorted(["x", "z", "e", "r", "r2", "kd", "kd2", "q", "w",
                              "late1", "late5", "late6", "late8", "late9"]), entries
    assert os.stat("%s/e" % mount).st_mtime_ns // 1000 == base + 55
    for name, content in [("r", b"abc"), ("r2", b""), ("kd/f", b"kept"),
                          ("kd2/f", b"\0" * 3), ("q", b""), ("w", b"\0" * 7)]:
        with open("%s/%s" % (mount, name), "rb") as f:
            data = f.read()
        assert data == content, (name, data)

check()

# the same after a remount, which applies every action in the order of time
bucseTests.verifyFailOnError(allowedErrors = [
    # rejected, as on every client: the removeFile of step 2 and the
    # removeDirectory of step 3 come before the file or directory is added
    "doAction: file not found: z",
    "doAction: dir not found: d",
    # step 7
    "doAction: path not found when adding file kd2/f",
])
check()

bucseTests.testCleanup()

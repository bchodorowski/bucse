#!/bin/python3

# rename() with RENAME_EXCHANGE swaps two files: each path gets the other
# one's content and mtime. Both editFile actions are written in one action
# file, so no client sees only one of them applied. Pending writes are
# flushed first, and an open file goes on to be written under its new path.
# Exchanges that involve a directory aren't supported (EINVAL, see
# "Non-goals" in docs/design.md), and the other errors are the usual ones.
# An out-of-order action file (with two actions) undoes the exchange and
# applies it again.

import bucseTests
import ctypes
import hashlib
import errno
import json
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mount = "test_%d" % bucseTests.pid
mirror = "test_%d_mirror" % bucseTests.pid

AT_FDCWD = -100
RENAME_NOREPLACE = 1 << 0
RENAME_EXCHANGE = 1 << 1

libc = ctypes.CDLL(None, use_errno=True)
libc.renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]


# returns 0 or the errno of renameat2()
def renameat2(src, dst, flags):
    if libc.renameat2(AT_FDCWD, src.encode(), AT_FDCWD, dst.encode(), flags) == 0:
        return 0
    return ctypes.get_errno()


def exchange(src, dst):
    for d in [mount, mirror]:
        result = renameat2("%s/%s" % (d, src), "%s/%s" % (d, dst), RENAME_EXCHANGE)
        assert result == 0, (d, src, dst, os.strerror(result))


def writeFile(path, data):
    for d in [mount, mirror]:
        with open("%s/%s" % (d, path), "wb") as f:
            f.write(data)


def mtime(path):
    return os.stat("%s/%s" % (mount, path)).st_mtime_ns


os.mkdir("%s/d" % mount)
os.mkdir("%s/d" % mirror)
os.mkdir("%s/e" % mount)
os.mkdir("%s/e" % mirror)
writeFile("a", os.urandom(300 * 1024))
writeFile("b", b"b" * 1000)
writeFile("d/c", os.urandom(5000))
writeFile("empty", b"")

# 1. two files in the root: one action file with both editFiles; the mtimes
# are swapped too
mtimeA, mtimeB = mtime("a"), mtime("b")
assert mtimeA != mtimeB
actionsBefore = set(bucseTests.listRepoDir("actions"))
exchange("a", "b")
newActions = set(bucseTests.listRepoDir("actions")) - actionsBefore
assert len(newActions) == 1, newActions
assert (mtime("a"), mtime("b")) == (mtimeB, mtimeA), (mtime("a"), mtime("b"), mtimeA, mtimeB)
if bucseTests.argEncryption == "none":
    actions = json.loads(bucseTests.readRepoFile("actions/%s" % newActions.pop()))
    assert sorted((a["action"], a["path"]) for a in actions) \
        == [("editFile", "a"), ("editFile", "b")], actions

# 2. files in different directories, and with an empty file
exchange("b", "d/c")
exchange("empty", "d/c")

# 3. a file with pending writes: they are flushed before the exchange, and
# the open file is written under its new path afterwards
holder = bucseTests.FileHolder()
fd = holder.open("%s/a" % mount, os.O_WRONLY)
fdMirror = os.open("%s/a" % mirror, os.O_WRONLY)
for f, w in [(fd, holder.write), (fdMirror, lambda fd, data: os.write(fd, data))]:
    w(f, b"before the exchange")
exchange("a", "d/c")
for f, w in [(fd, holder.write), (fdMirror, lambda fd, data: os.write(fd, data))]:
    w(f, b", after it")
holder.close(fd)
os.close(fdMirror)
holder.stop()

# 4. a file with itself: nothing changes
exchange("a", "a")

# 5. undo: another client's action file arrives late, with two actions: an
# editFile of p that is older than the exchange of p and q, and an
# addDirectory. Both editFiles of the exchange are undone, the late actions
# applied, and the exchange applied again, as on a remount: it carries the
# content, so p gets q's content, not the late edit (an empty file), and the
# mtimes stay swapped
writeFile("p", os.urandom(4000))
writeFile("q", os.urandom(7000))
mtimeP, mtimeQ = mtime("p"), mtime("q")
lateTime = time.time_ns() // 1000
time.sleep(0.01)
exchange("p", "q")
os.mkdir("%s/lateDir" % mirror)
data = json.dumps([
    {"time": lateTime, "action": "editFile", "path": "p",
     "content": [], "size": 0, "blockSize": 0},
    {"time": lateTime, "action": "addDirectory", "path": "lateDir",
     "content": [], "size": 0, "blockSize": 0}]).encode("utf-8")
bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
    bucseTests.encryptForRepo(data))
bucseTests.waitForFiles(["lateDir"])
bucseTests.waitForActionsHandled()
with open("tmp/mount_%d.txt" % bucseTests.pid, "rb") as f:
    assert b"undoing action due to out of order action" in f.read()
for path in ["p", "q"]:
    with open("%s/%s" % (mount, path), "rb") as f, open("%s/%s" % (mirror, path), "rb") as fMirror:
        assert f.read() == fMirror.read(), path
assert (mtime("p"), mtime("q")) == (mtimeQ, mtimeP)
# the marker of waitForActionsHandled() isn't in the mirror
os.rmdir("%s/marker_%d" % (mount, bucseTests.markerCount))

# 6. errors: a directory, a missing destination, unsupported flags
for src, dst, flags, expected in [
        ("a", "d", RENAME_EXCHANGE, errno.EINVAL),
        ("d", "a", RENAME_EXCHANGE, errno.EINVAL),
        ("d", "e", RENAME_EXCHANGE, errno.EINVAL),
        ("a", "missing", RENAME_EXCHANGE, errno.ENOENT),
        ("missing", "a", RENAME_EXCHANGE, errno.ENOENT),
        ("a", "b", RENAME_EXCHANGE | RENAME_NOREPLACE, errno.EINVAL)]:
    result = renameat2("%s/%s" % (mount, src), "%s/%s" % (mount, dst), flags)
    assert result == expected, (src, dst, flags, os.strerror(result))

# the remount reads the action files with two actions back
bucseTests.verifyWithMirror()

bucseTests.verifyFailOnError(allowedErrors = ["exchange of directories is not supported",
    "exchange but destination"])

bucseTests.testCleanup()

#!/bin/python3

# utimens: setting the times of files and directories (touch, cp -p, tar x)
# writes a setTimes action, so other clients and a remount see them, in
# nanoseconds. A file with local changes is flushed first. A renamed file or
# directory keeps its mtime. Another client's setTimes is applied, and
# rejected for a file that isn't there; an older one that arrives late doesn't
# override a newer one.

import bucseTests
import errno
import hashlib
import json
import os
import subprocess
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

mount = "test_%d" % bucseTests.pid

T1 = 981173106123456789    # 2001-02-03 04:05:06.123456789 UTC
T2 = 1234567890987654321
T3 = -1500000000           # before the epoch
T4 = 1600000000000000001


def times(name):
    st = os.stat("%s/%s" % (mount, name) if name else mount)
    return st.st_atime_ns, st.st_mtime_ns


# 1. touch of a new file, and of an existing one
before = time.time_ns()
subprocess.run(["touch", "%s/new" % mount], check=True)
atime, mtime = times("new")
assert before - 10**9 < mtime < time.time_ns() + 10**9, (before, mtime)

with open("%s/f" % mount, "wb") as f:
    f.write(b"data")
os.utime("%s/f" % mount, ns=(T2, T1))
assert times("f") == (T2, T1), times("f")

# only one of the times
subprocess.run(["touch", "-m", "-d", "@1600000000.000000001", "%s/f" % mount], check=True)
assert times("f") == (T2, T4), times("f")

# 2. like cp -p: the times are set before the written file is closed (the
# local changes are flushed first, the setTimes comes after them)
fd = os.open("%s/cp" % mount, os.O_CREAT | os.O_WRONLY)
os.write(fd, b"copied")
os.utime(fd, ns=(T1, T2))
os.close(fd)
assert times("cp") == (T1, T2), times("cp")

# 3. a directory, and the root directory
os.mkdir("%s/d" % mount)
with open("%s/d/inner" % mount, "wb") as f:
    f.write(b"inner")
os.utime("%s/d/inner" % mount, ns=(T1, T1))
os.utime("%s/d" % mount, ns=(T3, T3))
assert times("d") == (T3, T3), times("d")
os.utime(mount, ns=(T2, T2))
assert times("")[1] == T2, times("")

# 4. a time that doesn't fit in int64 nanoseconds
try:
    os.utime("%s/f" % mount, ns=(T1, 10**19 * 3))
    assert False, "expected EOVERFLOW"
except OSError as e:
    assert e.errno == errno.EOVERFLOW, e

# 5. rename keeps the mtime: a file, a file over an existing one, and a
# directory with a file in it
os.rename("%s/cp" % mount, "%s/cp2" % mount)
assert times("cp2")[1] == T2, times("cp2")
with open("%s/over" % mount, "wb") as f:
    f.write(b"old")
os.rename("%s/cp2" % mount, "%s/over" % mount)
assert times("over")[1] == T2, times("over")
os.rename("%s/d" % mount, "%s/d2" % mount)
assert times("d2")[1] == T3, times("d2")
assert times("d2/inner")[1] == T1, times("d2/inner")

# 6. another client's setTimes; one for a file that isn't there is rejected
base = int(time.time() * 1000000) - 3600 * 1000000

def writeAction(actionType, path, offset, **fields):
    action = {"time": base + offset, "action": actionType, "path": path,
              "content": [], "size": 0, "blockSize": 0}
    action.update(fields)
    data = json.dumps([action]).encode("utf-8")
    bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
        bucseTests.encryptForRepo(data))

writeAction("addFile", "remote", 10, mtime=T4)
writeAction("setTimes", "gone", 15, mtime=T1)
bucseTests.waitForActionsHandled()
assert times("remote")[1] == T4, times("remote")
writeAction("setTimes", "remote", 30, mtime=T1, atime=T2)
bucseTests.waitForActionsHandled()
assert times("remote") == (T2, T1), times("remote")

# an older setTimes that arrives late doesn't override the newer one
writeAction("setTimes", "remote", 20, mtime=T3)
bucseTests.waitForActionsHandled()
assert times("remote") == (T2, T1), times("remote")


# (the atime of a renamed entry isn't saved, and a readdir changes a
# directory's atime, so only the mtime is checked for them)
def check():
    assert times("f") == (T2, T4), times("f")
    assert times("over")[1] == T2, times("over")
    assert times("d2")[1] == T3, times("d2")
    assert times("d2/inner")[1] == T1, times("d2/inner")
    assert times("")[1] == T2, times("")
    assert times("remote") == (T2, T1), times("remote")
    with open("%s/over" % mount, "rb") as f:
        assert f.read() == b"copied"
    assert not os.path.exists("%s/gone" % mount)

check()

bucseTests.verifyFailOnError(allowedErrors = [
    "doAction: path not found when setting times of gone",
])
check()

bucseTests.testCleanup()

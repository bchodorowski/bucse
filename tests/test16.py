#!/bin/python3

# FUSE calls create (not open) when the kernel thinks a file doesn't exist,
# but bucse-mount may know it by then, e.g. when another client added it.
# bucse_create() then opens the existing file; it used to lock bucseMutex a
# second time there and deadlock. The kernel is made to think so by caching
# the "doesn't exist" lookup result (negative_timeout) before the file is
# added with an action file.

import bucseTests
import os
import threading
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs(["-o", "negative_timeout=600"])

name = "file"
path = "test_%d/%s" % (bucseTests.pid, name)

# the kernel caches that the file doesn't exist
assert not os.path.exists(path)

# another client adds the (empty) file
actionName, data = bucseTests.makeAddDirectoryAction(name, "addFile")
bucseTests.writeRepoFile("actions/%s" % actionName, data)
# not waitForActionsHandled(): with negative_timeout, its marker directory
# would stay "missing" too. A directory listing isn't cached.
deadline = time.time() + 30
while name not in os.listdir("test_%d" % bucseTests.pid):
    assert time.time() < deadline, "timeout waiting for the file to be added"
    time.sleep(0.2)

# open the file in a thread, so that a deadlock fails the test instead of
# hanging it
result = {}
def openAndWrite():
    fd = os.open(path, os.O_WRONLY | os.O_CREAT)
    os.write(fd, b"data")
    os.close(fd)
    result["done"] = True
thread = threading.Thread(target = openAndWrite, daemon = True)
thread.start()
thread.join(30)
if not result.get("done"):
    # the mount is stuck, kill it so that the test can exit
    if bucseTests.valgrindProc:
        bucseTests.valgrindProc.kill()
        bucseTests.valgrindProc.wait()
    bucseTests.unmount()
    raise Exception("opening an existing file with O_CREAT didn't finish (deadlock in bucse_create?)")

with open(path, "rb") as f:
    assert f.read() == b"data"

bucseTests.verifyFailOnError()

# the same after a remount (verifyFailOnError() remounts)
with open(path, "rb") as f:
    assert f.read() == b"data"

bucseTests.testCleanup()

#!/bin/python3

# When mounting fails (here: the mountpoint doesn't exist), bucse-mount exits
# with an error without joining the destination's tick thread, which was never
# created (it used to call pthread_join() on it anyway).

import bucseTests
import subprocess


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, bucseTests.pid)
mountpoint = "tmp/test18_nonexistent_%d" % bucseTests.pid
argsList = ["../bucse-mount", "-p", bucseTests.argPassphrase, "-r", repo, "-f", mountpoint]
if bucseTests.argValgrind:
    # only memory errors, a failed mount may leave things allocated
    argsList = ["valgrind", "--error-exitcode=99", "--leak-check=no"] + argsList
p = subprocess.run(argsList, capture_output=True, timeout=60)
output = (p.stdout + p.stderr).decode("utf-8", errors="replace")
assert p.returncode > 0 and p.returncode != 99, \
    "bucse-mount returned %d: %s" % (p.returncode, output)
assert "bad mount point" in output, output
assert "pthread_join" not in output, output

# the mount made by mountDirs() still works
bucseTests.waitForActionsHandled()

bucseTests.verifyFailOnError(True)
bucseTests.testCleanup()

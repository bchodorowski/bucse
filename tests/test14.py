#!/bin/python3

# The check interval (-i, -o check_interval) must be at least 1 second:
# bucse-mount refuses to mount with a smaller one. The tests mount with -i 1
# (see mountDirs()), so the interval itself is used by every test.

import bucseTests
import os
import subprocess


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, bucseTests.pid)
mountpoint = "tmp/test14_mount_%d" % bucseTests.pid
os.mkdir(mountpoint)
try:
    for option in [["-i", "0"], ["-i", "-5"], ["-o", "check_interval=0"]]:
        p = subprocess.run(["../bucse-mount"] + bucseTests.passphraseArgs() + ["-r", repo, "-f"]
                           + option + [mountpoint],
                           capture_output=True, timeout=30)
        assert p.returncode != 0, "%s accepted" % option
        assert b"check interval must be at least 1 second" in p.stdout + p.stderr, \
            "%s: %s" % (option, p.stdout + p.stderr)
finally:
    os.rmdir(mountpoint)

# the mount made by mountDirs() picks up new action files
bucseTests.waitForActionsHandled()

bucseTests.verifyFailOnError(True)
bucseTests.testCleanup()

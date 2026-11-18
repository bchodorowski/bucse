#!/bin/python3

# bucse-mount refuses to mount a repository whose repository.json or
# repository file isn't valid JSON, and logs the parse error without using
# freed memory (the json tokener used to be freed before the error message
# was built from it).

import bucseTests
import os
import subprocess


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, bucseTests.pid)
mountpoint = "tmp/test17_mount_%d" % bucseTests.pid
os.mkdir(mountpoint)
try:
    for fileName, broken, expectedError in [
            ("repository.json", b"{\"encryption\": ", "parseRepositoryJsonFile() failed"),
            ("repository", bucseTests.encryptForRepo(b"{\"time\": "), "parseRepositoryFile() failed")]:
        original = bucseTests.readRepoFile(fileName)
        bucseTests.writeRepoFile(fileName, broken)
        try:
            argsList = ["../bucse-mount", "-p", bucseTests.argPassphrase, "-r", repo, "-f", mountpoint]
            if bucseTests.argValgrind:
                # only memory errors, a failed mount may leave things allocated
                argsList = ["valgrind", "--error-exitcode=99", "--leak-check=no"] + argsList
            p = subprocess.run(argsList, capture_output=True, timeout=60)
        finally:
            bucseTests.writeRepoFile(fileName, original)
        output = (p.stdout + p.stderr).decode("utf-8", errors="replace")
        assert p.returncode != 0 and p.returncode != 99, \
            "%s: bucse-mount returned %d: %s" % (fileName, p.returncode, output)
        assert "json_tokener_parse_ex(): " in output and expectedError in output, \
            "%s: %s" % (fileName, output)
finally:
    os.rmdir(mountpoint)

# the mount made by mountDirs() still works
bucseTests.waitForActionsHandled()

bucseTests.verifyFailOnError(True)
bucseTests.testCleanup()

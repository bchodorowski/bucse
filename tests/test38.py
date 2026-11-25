#!/bin/python3

# ssh destination: when the host accepts none of the public keys (e.g. the key
# isn't in authorized_keys), bucse-init and bucse-mount fail with an error
# that says so, not with a misleading one from a later step (it used to be
# "Error allocating SFTP session: sftp_new: Out of memory"). Tested by logging
# in as a user that doesn't exist on the test host. Needs an ssh repository
# path; with a local one there's nothing to test.

import bucseTests
import os
import re
import subprocess


bucseTests.parseArgs()

pid = bucseTests.pid

if not bucseTests.parseSshRepoPath():
    print("not an ssh repository, nothing to test")
    exit(0)

# the same host and path, as a user that doesn't exist there
r = re.match(r'ssh://(?:[^@/]*@)?(.*)$', bucseTests.argRepoPath)
repo = "ssh://bucseNoSuchUser%d@%s/test_%d_repo" % (pid, r.group(1), pid)
mountpoint = "tmp/test38_mount_%d" % pid
os.makedirs(mountpoint, exist_ok=True)


def check(name, args):
    p = subprocess.run(args, capture_output=True, stdin=subprocess.DEVNULL, timeout=60)
    output = (p.stdout + p.stderr).decode("utf-8", errors="replace")
    if p.returncode == 0:
        raise Exception("%s succeeded without authentication: %s" % (name, output))
    if "accepted none of the public keys" not in output or "SFTP" in output:
        raise Exception("%s: unexpected error: %s" % (name, output))


try:
    check("bucse-init", ["../bucse-init"] + bucseTests.passphraseArgs()
        + ["-e", bucseTests.argEncryption, repo])
    check("bucse-mount", ["../bucse-mount", "-f"] + bucseTests.passphraseArgs()
        + ["-r", repo, mountpoint])
finally:
    os.rmdir(mountpoint)

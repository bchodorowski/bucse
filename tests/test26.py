#!/bin/python3

# bucse-init fails (non-zero exit code) without a repository argument, with an
# option that is missing its argument (and says so), and when it can't create
# the repository.

import bucseTests
import subprocess


bucseTests.parseArgs()


def init(args):
    p = subprocess.run(["../bucse-init"] + args, capture_output=True)
    return p.returncode, p.stdout.decode()

repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, bucseTests.pid)

# bucse-init creates only the repository directory, not its parents
ssh = bucseTests.parseSshRepoPath()
if ssh:
    sshArgs, repoPath = ssh
    subprocess.run(sshArgs + ["mkdir -p %s" % bucseTests.remoteShellPath(repoPath)], check=True)

try:
    # no repository
    code, out = init(["-e", "none"])
    assert code != 0, out
    assert "No repository given" in out, out

    # an option without its argument
    for option in ["-p", "-P", "-e", "-n", "-c"]:
        code, out = init([option])
        assert code != 0, (option, out)
        assert "Option %s requires an argument" % option in out, (option, out)

    # an unknown option
    code, out = init(["-x", repo])
    assert code != 0, out
    assert "Unknown option `-x'" in out, out

    # a repository that can be created
    code, out = init(["-e", bucseTests.argEncryption]
        + bucseTests.passphraseArgs() + [repo])
    assert code == 0, out

    # the same repository again: it already exists
    code, out = init(["-e", bucseTests.argEncryption]
        + bucseTests.passphraseArgs() + [repo])
    assert code != 0, out
finally:
    bucseTests.removeRepo()

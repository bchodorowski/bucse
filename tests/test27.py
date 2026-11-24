#!/bin/python3

# When bucse-mount asks for the passphrase on a terminal and reading it fails
# (here: EOF), it restores the terminal's echo before exiting (it used to
# leave echo turned off).

import bucseTests
import os
import pty
import select
import subprocess
import termios
import time


bucseTests.parseArgs()

if bucseTests.argEncryption == "none":
    # the none backend doesn't ask for a passphrase
    exit(0)

repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, bucseTests.pid)
mountpoint = "test_%d" % bucseTests.pid

# bucse-init creates only the repository directory, not its parents
ssh = bucseTests.parseSshRepoPath()
if ssh:
    sshArgs, repoPath = ssh
    subprocess.run(sshArgs + ["mkdir -p %s" % bucseTests.remoteShellPath(repoPath)], check=True)

master, slave = pty.openpty()
try:
    subprocess.run(["../bucse-init", "-e", bucseTests.argEncryption]
        + bucseTests.passphraseArgs() + [repo], check=True)
    os.mkdir(mountpoint)

    assert termios.tcgetattr(slave)[3] & termios.ECHO

    # no passphrase option: bucse-mount asks for the passphrase
    argsList = ["../bucse-mount", "-r", repo, "-f", mountpoint]
    if bucseTests.argValgrind:
        # only memory errors, a failed mount may leave things allocated
        argsList = ["valgrind", "--error-exitcode=99", "--leak-check=no"] + argsList
    p = subprocess.Popen(argsList, stdin=slave, stdout=slave, stderr=slave)

    # wait for the prompt
    output = b""
    deadline = time.time() + 60
    while b"Passphrase: " not in output:
        assert time.time() < deadline, output
        if select.select([master], [], [], 1)[0]:
            output += os.read(master, 4096)

    # echo is off while the passphrase is read (it's turned off just after
    # the prompt is printed)
    while termios.tcgetattr(slave)[3] & termios.ECHO:
        assert time.time() < deadline, output
        time.sleep(0.1)

    # EOF at the start of a line: no passphrase
    os.write(master, b"\x04")
    p.wait(timeout=60)

    while select.select([master], [], [], 0.2)[0]:
        try:
            output += os.read(master, 4096)
        except OSError:
            break
    output = output.decode("utf-8", errors="replace")

    assert p.returncode > 0 and p.returncode != 99, \
        "bucse-mount returned %d: %s" % (p.returncode, output)
    assert "Encryption needs a passphrase" in output, output
    assert termios.tcgetattr(slave)[3] & termios.ECHO, output
finally:
    os.close(master)
    os.close(slave)
    if os.path.isdir(mountpoint):
        os.rmdir(mountpoint)
    bucseTests.removeRepo()

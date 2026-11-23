#!/bin/python3

# Without -p, bucse-mount asks for the passphrase on the terminal (with echo
# off) when the encryption backend needs one. The test runs it on a pty and
# checks: the correct passphrase mounts the repository, a wrong one is
# rejected, an empty one and end of input (Ctrl+D) fail cleanly. With the
# `none` backend there is no prompt.

import bucseTests
import os
import pty
import subprocess
import threading
import time


bucseTests.parseArgs()

bucseTests.failOnError = True

bucseTests.mountDirs()

mountDir = "test_%d" % bucseTests.pid
with open(mountDir + "/hello", "w") as f:
    f.write("hello world")
bucseTests.waitForActionsHandled()

repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, bucseTests.pid)
mountpoint = "tmp/test31_mount_%d" % bucseTests.pid
needsPassphrase = bucseTests.argEncryption != "none"


# Starts bucse-mount (in the foreground) with its stdin, stdout and stderr on
# a pty. Returns the process, the pty's master fd and a list that a thread
# fills with everything bucse-mount writes to the pty.
def startOnPty():
    argsList = ["../bucse-mount", "-r", repo, "-f", mountpoint]
    if bucseTests.argValgrind:
        argsList = ["valgrind", "--log-file=tmp/valgrind_test31_%d.txt" % bucseTests.pid,
                    "--error-exitcode=99", "--leak-check=full", "--show-leak-kinds=all",
                    "--errors-for-leak-kinds=all", "--num-callers=30"] + argsList
    master, slave = pty.openpty()
    p = subprocess.Popen(argsList, stdin=slave, stdout=slave, stderr=slave)
    os.close(slave)
    output = []

    def reader():
        while True:
            try:
                data = os.read(master, 4096)
            except OSError:
                # EIO: bucse-mount has exited and closed the pty
                return
            if not data:
                return
            output.append(data)

    threading.Thread(target=reader, daemon=True).start()
    return p, master, output


def outputText(output):
    return b"".join(output).decode("utf-8", errors="replace")


def waitForOutput(output, text, timeout = 60):
    deadline = time.time() + timeout
    while text not in outputText(output):
        if time.time() > deadline:
            raise Exception("timed out waiting for %r, output: %r" % (text, outputText(output)))
        time.sleep(0.1)


def waitForExit(p, master, output):
    try:
        p.wait(timeout=120)
    finally:
        time.sleep(0.2)
        os.close(master)
    if bucseTests.argValgrind:
        assert p.returncode != 99, "valgrind found errors, see tmp/valgrind_test31_%d.txt" % bucseTests.pid
        os.remove("tmp/valgrind_test31_%d.txt" % bucseTests.pid)
    return outputText(output)


os.mkdir(mountpoint)
try:
    if needsPassphrase:
        # typed input that isn't accepted: (input, expected error)
        for typed, expectedError in [
                (bucseTests.argPassphrase + "wrong\n", "Is the passphrase correct?"),
                ("\n", "Encryption needs a passphrase"),
                ("\x04", "fgets() failed")]:
            p, master, output = startOnPty()
            waitForOutput(output, "Passphrase: ")
            os.write(master, typed.encode())
            text = waitForExit(p, master, output)
            assert p.returncode != 0, "%r: bucse-mount returned 0: %r" % (typed, text)
            assert expectedError in text, "%r: %r" % (typed, text)

    p, master, output = startOnPty()
    if needsPassphrase:
        waitForOutput(output, "Passphrase: ")
        os.write(master, (bucseTests.argPassphrase + "\n").encode())
    bucseTests.waitForRepoToBeMounted(mountpoint)
    with open(mountpoint + "/hello") as f:
        assert f.read() == "hello world"
    bucseTests.unmount(mountpoint)
    text = waitForExit(p, master, output)
    assert p.returncode == 0, "bucse-mount returned %d: %r" % (p.returncode, text)
    if needsPassphrase:
        # echo was off
        assert bucseTests.argPassphrase not in text, text
    else:
        assert "Passphrase" not in text, text
finally:
    subprocess.run(["umount", mountpoint], stderr=subprocess.DEVNULL)
    os.rmdir(mountpoint)

bucseTests.verifyFailOnError()
bucseTests.testCleanup()

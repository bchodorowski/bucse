#!/bin/python3

# The passphrase is kept off the command line:
# - bucse-init and bucse-mount read it from a file with -P FILE (also
#   --passphrase-file=FILE, and -o passphrase_file=FILE for bucse-mount; '-'
#   is stdin), the first line without its newline. A missing or empty file,
#   or a too long passphrase, is an error.
# - bucse-init asks for it on a terminal twice, and fails if the two don't
#   match (bucse-mount's prompt is tested by test31).
# - -p and -o passphrase= still work, with a warning, and bucse-mount zeroes
#   the passphrase in its argv, so /proc/<pid>/cmdline (and ps) no longer
#   shows it.
# - -p together with -P is an error.

import bucseTests
import os
import pty
import subprocess
import threading
import time


bucseTests.parseArgs()

pid = bucseTests.pid
passphrase = bucseTests.argPassphrase
needsPassphrase = bucseTests.argEncryption != "none"
repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, pid)
mountpoint = "tmp/test36_mount_%d" % pid


def tmpFile(name, content):
    bucseTests.tmpFiles.append("test36_%s_%d" % (name, pid))
    path = "tmp/test36_%s_%d" % (name, pid)
    with open(path, "w") as f:
        f.write(content)
    return path


def createRepoParent():
    # bucse-init creates only the repository directory, not its parents
    ssh = bucseTests.parseSshRepoPath()
    if ssh:
        sshArgs, repoPath = ssh
        subprocess.run(sshArgs + ["mkdir -p %s" % bucseTests.remoteShellPath(repoPath)], check=True)


def outputText(output):
    return b"".join(output).decode("utf-8", errors="replace")


# Runs args on a pty (stdin, stdout and stderr). prompts: (prompt, answer),
# the answer is typed once the prompt shows up. Returns the exit code and the
# output.
def runOnPty(args, prompts):
    master, slave = pty.openpty()
    p = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave)
    os.close(slave)
    output = []

    def reader():
        while True:
            try:
                data = os.read(master, 4096)
            except OSError:
                # EIO: the process has exited and closed the pty
                return
            if not data:
                return
            output.append(data)

    threading.Thread(target=reader, daemon=True).start()
    try:
        for prompt, answer in prompts:
            deadline = time.time() + 60
            while prompt not in outputText(output):
                assert time.time() < deadline, outputText(output)
                time.sleep(0.1)
            os.write(master, answer.encode())
        p.wait(timeout=60)
    finally:
        time.sleep(0.2)
        os.close(master)
    return p.returncode, outputText(output)


def init(args, stdin = subprocess.DEVNULL):
    p = subprocess.run(["../bucse-init", "-e", bucseTests.argEncryption] + args + [repo],
        stdin=stdin, capture_output=True, timeout=60)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", errors="replace")


# bucse-init

createRepoParent()
try:
    # -p works, with a warning
    code, out = init(["-p", passphrase])
    assert code == 0, out
    assert "[warning]: a passphrase given on the command line" in out, out
    bucseTests.removeRepo()

    # -p and -P together
    code, out = init(["-p", passphrase] + bucseTests.passphraseArgs())
    assert code != 0, out
    assert "either -p or -P" in out, out

    if needsPassphrase:
        # no passphrase, not on a terminal
        code, out = init([])
        assert code != 0, out
        assert "Password is necessary" in out, out

        # a missing file
        code, out = init(["-P", "tmp/test36_nonexistent_%d" % pid])
        assert code != 0, out
        assert "can't open the passphrase file" in out, out

        # empty stdin
        code, out = init(["--passphrase-file=-"])
        assert code != 0, out
        assert "no passphrase on stdin" in out, out

        # the two typed passphrases don't match: no repository
        code, out = runOnPty(["../bucse-init", "-e", bucseTests.argEncryption, repo],
            [("Passphrase: ", passphrase + "\n"),
             ("Repeat the passphrase: ", passphrase + "x\n")])
        assert code != 0, out
        assert "the passphrases don't match" in out, out
        assert passphrase not in out, "echo was on: %r" % out

        # matching ones: the repository is created with that passphrase
        # (mounted with it below)
        code, out = runOnPty(["../bucse-init", "-e", bucseTests.argEncryption, repo],
            [("Passphrase: ", passphrase + "\n"),
             ("Repeat the passphrase: ", passphrase + "\n")])
        assert code == 0, out
        assert passphrase not in out, "echo was on: %r" % out
    else:
        # no prompt for the none backend
        code, out = runOnPty(["../bucse-init", "-e", bucseTests.argEncryption, repo], [])
        assert code == 0, out
        assert "Passphrase" not in out, out
    bucseTests.removeRepo()
except:
    bucseTests.removeRepo()
    raise


# bucse-mount

bucseTests.failOnError = True
bucseTests.mountDirs()

mountDir = "test_%d" % pid
with open(mountDir + "/hello", "w") as f:
    f.write("hello world")
bucseTests.waitForActionsHandled()


# Mounts the repository at mountpoint with args, checks that it works and
# unmounts it. Returns bucse-mount's output and its argv as
# /proc/<pid>/cmdline shows it while mounted. valgrind: run it under valgrind
# if the tests use it (under valgrind, /proc/<pid>/cmdline is valgrind's).
def mount(args, valgrind = True, stdin = subprocess.DEVNULL):
    argsList = ["../bucse-mount", "-r", repo, "-f"] + args + [mountpoint]
    underValgrind = valgrind and bucseTests.argValgrind
    if underValgrind:
        argsList = ["valgrind", "--log-file=tmp/valgrind_test36_%d.txt" % pid,
                    "--error-exitcode=99", "--leak-check=full", "--show-leak-kinds=all",
                    "--errors-for-leak-kinds=all", "--num-callers=30"] + argsList
    with open("tmp/test36_out_%d" % pid, "w+b") as out:
        p = subprocess.Popen(argsList, stdin=stdin, stdout=out, stderr=subprocess.STDOUT)
        try:
            bucseTests.waitForRepoToBeMounted(mountpoint, p)
            with open("/proc/%d/cmdline" % p.pid, "rb") as f:
                cmdline = f.read()
            with open(mountpoint + "/hello") as f:
                assert f.read() == "hello world"
            bucseTests.unmount(mountpoint)
            p.wait(timeout=60)
        finally:
            if p.poll() is None:
                subprocess.run(["umount", mountpoint], stderr=subprocess.DEVNULL)
                p.wait(timeout=60)
        out.seek(0)
        text = out.read().decode("utf-8", errors="replace")
    os.remove("tmp/test36_out_%d" % pid)
    assert p.returncode == 0, "%r: bucse-mount returned %d: %s" % (args, p.returncode, text)
    if underValgrind:
        os.remove("tmp/valgrind_test36_%d.txt" % pid)
    return text, cmdline


# Runs bucse-mount with args, which must fail with expectedError.
def mountFails(args, expectedError, stdin = subprocess.DEVNULL):
    argsList = ["../bucse-mount", "-r", repo, "-f"] + args + [mountpoint]
    if bucseTests.argValgrind:
        # only memory errors, a failed mount may leave things allocated
        argsList = ["valgrind", "--error-exitcode=99", "--leak-check=no"] + argsList
    p = subprocess.run(argsList, stdin=stdin, capture_output=True, timeout=60)
    text = (p.stdout + p.stderr).decode("utf-8", errors="replace")
    assert p.returncode > 0 and p.returncode != 99, \
        "%r: bucse-mount returned %d: %s" % (args, p.returncode, text)
    assert expectedError in text, (args, text)


os.mkdir(mountpoint)
try:
    # -p and -o passphrase=: mounts, with a warning, and the passphrase is
    # zeroed in argv. (args, the same with the passphrase zeroed)
    zeroed = "\0" * len(passphrase)
    cases = [
        (["-p", passphrase], ["-p", zeroed]),
        (["-p" + passphrase], ["-p" + zeroed]),
        (["-o", "ro,passphrase=%s,check_interval=1" % passphrase],
         ["-o", "ro,passphrase=%s,check_interval=1" % zeroed]),
        (["-opassphrase=" + passphrase], ["-opassphrase=" + zeroed])]
    if not needsPassphrase:
        # an escaped comma is part of the value: the passphrase is
        # "<passphrase>,x", wrong for aes
        cases.append((["-o", "passphrase=%s\\,x,ro" % passphrase],
                      ["-o", "passphrase=%s,ro" % ("\0" * (len(passphrase) + 3))]))
    for args, hidden in cases:
        # under valgrind, /proc/<pid>/cmdline is valgrind's, not the argv
        # bucse-mount changes
        text, cmdline = mount(args, valgrind = False)
        assert "[warning]: a passphrase given on the command line" in text, (args, text)
        expected = ["../bucse-mount", "-r", repo, "-f"] + hidden + [mountpoint]
        assert cmdline == ("\0".join(expected) + "\0").encode(), (args, cmdline)

    # a passphrase file, in every form: mounts, no warning. The second line
    # is ignored.
    passFile = tmpFile("pass", passphrase + "\nsomething else\n")
    noNewlineFile = tmpFile("passNoNewline", passphrase)
    for args, stdin in [
            (["-P", passFile], subprocess.DEVNULL),
            (["-P" + noNewlineFile], subprocess.DEVNULL),
            (["--passphrase-file=" + passFile], subprocess.DEVNULL),
            (["-o", "passphrase_file=" + passFile], subprocess.DEVNULL),
            (["-P", "-"], subprocess.PIPE)]:
        if stdin == subprocess.PIPE:
            stdin = open(passFile)
        text, cmdline = mount(args, stdin = stdin)
        if stdin != subprocess.DEVNULL:
            stdin.close()
        assert "[warning]" not in text, (args, text)

    # -p and -P together
    mountFails(["-p", passphrase, "-P", passFile], "not both")

    if needsPassphrase:
        tooLong = "x" * 1024
        for args, expectedError in [
                (["-P", "tmp/test36_nonexistent_%d" % pid], "can't open the passphrase file"),
                (["-P", tmpFile("empty", "")], "no passphrase in"),
                # only the first line counts
                (["-P", tmpFile("emptyLine", "\n" + passphrase + "\n")], "no passphrase in"),
                (["-P", tmpFile("wrong", passphrase + "x\n")], "Is the passphrase correct?"),
                (["-P", tmpFile("tooLong", tooLong + "\n")], "longer than 1023 bytes"),
                # stdin is /dev/null
                (["-P", "-"], "no passphrase on stdin")]:
            mountFails(args, expectedError)

        # 1023 bytes is the longest allowed: it's read (and wrong)
        mountFails(["-P", tmpFile("longest", "x" * 1023)], "Is the passphrase correct?")
finally:
    subprocess.run(["umount", mountpoint], stderr=subprocess.DEVNULL)
    os.rmdir(mountpoint)

bucseTests.verifyFailOnError()
bucseTests.testCleanup()

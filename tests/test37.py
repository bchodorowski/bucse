#!/bin/python3

# bucse-archive REPOSITORY packs the action files of a repository (local,
# file:// or ssh://, like bucse-mount's -r) into one tar file in actions/ and
# removes them; tar files already there are left alone, and so are files
# modified in the last 5 minutes (they may still be being uploaded). The
# repository's contents stay the same, also for a client that has it mounted
# meanwhile: it already has the packed actions, and ignores them when it reads
# the tar file (duplicates are found by their contents, not by the file name),
# and doesn't log an error for a file removed after it listed the directory,
# or for a tar file being written (bucse-archive writes it under a name that
# starts with a dot, and renames it when it is complete). Files whose name
# starts with a dot (ignored by bucse-mount, e.g. a temporary tar file left by
# an interrupted run) aren't packed. A directory that isn't a repository is
# refused, and nothing in it removed.

import bucseTests
import os
import subprocess


bucseTests.parseArgs()

pid = bucseTests.pid
repo = "%s/test_%d_repo" % (bucseTests.argRepoPath, pid)


def archive(repoArg):
    return subprocess.run(["../bucse-archive", repoArg], capture_output=True)


def actionFiles():
    return set(name for name in bucseTests.listRepoDir("actions") if not name.endswith(".tar"))


# Makes every file in actions/ look older than 5 minutes.
def ageActions():
    command = "find %s -type f -exec touch -d '10 minutes ago' {} +"
    ssh = bucseTests.parseSshRepoPath()
    if ssh:
        sshArgs, repoPath = ssh
        actionsPath = bucseTests.remoteShellPath("%s/test_%d_repo/actions" % (repoPath, pid))
        subprocess.run(sshArgs + [command % actionsPath], check=True)
    else:
        subprocess.run(["sh", "-c", command % "\"$1\"", "sh", repo + "/actions"], check=True)


def checkArchive(repoArg, expectedTars, expectedKept = set()):
    p = archive(repoArg)
    if p.returncode != 0:
        raise Exception("bucse-archive failed: %s" % p.stderr.decode())
    names = bucseTests.listRepoDir("actions")
    tars = [name for name in names if name.endswith(".tar")]
    kept = actionFiles()
    if len(tars) != expectedTars or kept != expectedKept:
        raise Exception("expected %d tar files and %s in actions/, got %s" % (expectedTars, expectedKept, names))


def makeChanges():
    for _ in range(16):
        bucseTests.mirrorCommand(["mkdir", bucseTests.getRandomNewFileName()])
    for _ in range(3):
        fileName = bucseTests.makeRandomTmpFile()
        targetDir = bucseTests.getRandomExistingDirName()
        bucseTests.mirrorCommand(["cp", "tmp/%s" % fileName, "%s/" % targetDir])


def waitForActionsHandled():
    bucseTests.waitForActionsHandled()
    # its marker directory, so that the mirror stays the same
    os.mkdir("test_%d_mirror/marker_%d" % (pid, bucseTests.markerCount))


def diffLive():
    subprocess.run(["diff", "-r", "test_%d_mirror" % pid, "test_%d" % pid], check=True)


# bucse-mount's log is checked for errors: a mounted client must handle the
# packing without any, e.g. for a file removed after it listed the directory,
# or a tar file that isn't complete yet
bucseTests.failOnError = True
bucseTests.mountDirs()

# packed while mounted; the action files of the last 5 minutes are kept
makeChanges()
ageActions()
old = actionFiles()
makeChanges()
fresh = actionFiles() - old
checkArchive(repo, 1, fresh)

# the mounted client reads the tar file: its actions (some of them its own)
# are already applied, so nothing changes, and new actions still work
makeChanges()
waitForActionsHandled()
diffLive()

# packed again and again while bucse-mount lists the directory every second
for tars in range(2, 6):
    makeChanges()
    ageActions()
    checkArchive(repo, tars)
waitForActionsHandled()
diffLive()
bucseTests.verifyFailOnError()

# a new client reads the tar files
bucseTests.verifyWithMirror()

# another tar file; the ones already there aren't packed again
makeChanges()
ageActions()
checkArchive(repo, 6)
bucseTests.verifyWithMirror()

# nothing to pack; a file whose name starts with a dot isn't packed
bucseTests.writeRepoFile("actions/.leftover.tmp", b"leftover")
ageActions()
checkArchive(repo, 6, {".leftover.tmp"})
bucseTests.deleteRepoFile("actions/.leftover.tmp")

# a local repository given as file://
if not bucseTests.parseSshRepoPath():
    makeChanges()
    ageActions()
    checkArchive("file://" + os.path.abspath(repo), 7)
    bucseTests.verifyWithMirror()

# not a repository: refused, its files are kept
notRepo = "tmp/test37_notRepo_%d" % pid
os.makedirs(notRepo + "/actions")
open(notRepo + "/actions/file", "w").close()
ageActionsCommand = ["touch", "-d", "10 minutes ago", notRepo + "/actions/file"]
subprocess.run(ageActionsCommand, check=True)
p = archive(notRepo)
kept = os.path.exists(notRepo + "/actions/file")
subprocess.run(["rm", "-rf", notRepo], check=True)
if p.returncode == 0 or not kept:
    raise Exception("bucse-archive didn't refuse a directory that isn't a repository")

bucseTests.testCleanup()

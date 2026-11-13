import os
import subprocess
import time
import random
import string
import argparse
import re
import shlex
import tarfile
import io
import atexit
import hashlib
import json

pid = os.getpid()

# temporary data files in tmp/, removed when the test exits (even if it fails)
tmpFiles = []
# logs in tmp/ (valgrind's, bucse-mount's output), removed only by
# testCleanup(), so they are kept when a test fails
logFiles = []


def removeTmpFiles():
    for tmpFile in tmpFiles:
        path = "tmp/%s" % tmpFile
        if os.path.exists(path):
            os.remove(path)

atexit.register(removeTmpFiles)

argDebug = False
argValgrind = False
argRepoPath = "."
argEncryption = "none"
argPassphrase = "12345"

valgrindProc = None

failOnError = False


def parseArgs():
    global argDebug
    global argValgrind
    global argRepoPath
    global argEncryption
    global argPassphrase

    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--debug",
        help="Do not run bucse-mount, just print the command that is intended to be run so it can be started with a gdb manually, then wait for newline on stdin to continue execution.",
        action="store_true")
    group.add_argument("--valgrind",
        help="Use valgrind.",
        action="store_true")
    parser.add_argument("--repo-path", "-r",
        help="Path where the repository will be placed.")
    parser.add_argument("--encryption", "-e",
        help="Encryption to be used. \"none\" or \"aes\".")
    parser.add_argument("--passphrase", "-p",
        help="Passphrase to be used.")
    args = parser.parse_args()
    if args.debug:
        argDebug = True
    if args.valgrind:
        argValgrind = True
    if args.repo_path:
        argRepoPath = args.repo_path
    if args.encryption:
        argEncryption = args.encryption
    if args.passphrase:
        argPassphrase = args.passphrase


def downloadTmp(url, filename, shasum):
    p = subprocess.run(["wget", "-O", "tmp/%s"%filename, url])
    p.check_returncode()

    p = subprocess.run(["shasum", "-c"], input=bytes("%s *tmp/%s\n"%(shasum, filename), 'UTF-8'))
    p.check_returncode()

    tmpFiles.append(filename)


# Unmounts the test directory. The kernel sends release requests for closed
# files asynchronously, so right after e.g. diff the mount may still be busy
# for a moment (especially under valgrind); retry for a while.
def unmount():
    for _ in range(60):
        p = subprocess.run(["umount", "test_%d" % pid])
        if p.returncode == 0:
            return
        time.sleep(0.5)
    p.check_returncode()


def waitForRepoToBeMounted(mountDir):
    while True:
        mountProcess = subprocess.run(['mount'], check=True, capture_output=True)
        grepProcess = subprocess.run(['grep', os.path.realpath(mountDir)],
                              input=mountProcess.stdout, capture_output=True)
        if grepProcess.returncode == 0:
            return
        time.sleep(0.2)


def mountDirs():
    global argDebug
    global argValgrind
    global argRepoPath
    global argEncryption
    global argPassphrase
    global valgrindProc
    global failOnError

    p = subprocess.run(["mkdir", "test_%d_mirror" % pid])
    p.check_returncode()

    p = subprocess.run(["mkdir", "test_%d" % pid])
    p.check_returncode()

    # bucse-init creates only the repository directory, not its parents
    ssh = parseSshRepoPath()
    if ssh:
        sshArgs, repoPath = ssh
        subprocess.run(sshArgs + ["mkdir -p %s" % remoteShellPath(repoPath)], check=True)

    p = subprocess.run(["../bucse-init", "-e", argEncryption, "-p", argPassphrase, "%s/test_%d_repo" % (argRepoPath, pid)])
    p.check_returncode()

    argsList = ["../bucse-mount", "-p", argPassphrase, "-r", "%s/test_%d_repo" % (argRepoPath, pid), "test_%d" % pid]

    if failOnError:
        argsList += ["-f", "-v 4"]

    # bucse-mount's output, checked for errors by verifyFailOnError(); a file,
    # not a pipe, so that a lot of output can't block bucse-mount
    mountOutput = None
    if failOnError:
        logFiles.append("mount_%d.txt" % pid)
        mountOutput = open("tmp/mount_%d.txt" % pid, "wb")
    else:
        mountOutput = subprocess.DEVNULL

    if argDebug:
        print(" ".join(argsList + ["-f -v 4"]))
        input()
    elif argValgrind:
        argsList = ["valgrind", "--log-file=tmp/valgrind_%d.txt" % pid, "--error-exitcode=-1", "--leak-check=full", "--show-leak-kinds=all", "--errors-for-leak-kinds=all", "--exit-on-first-error=yes"] + argsList + ["-f"]
        logFiles.append("valgrind_%d.txt" % pid)
        valgrindProc = subprocess.Popen(argsList, stdout=mountOutput, stderr=subprocess.STDOUT)
    elif failOnError:
        valgrindProc = subprocess.Popen(argsList, stdout=mountOutput, stderr=subprocess.STDOUT)
    else:
        p = subprocess.run(argsList)
        p.check_returncode()
    
    waitForRepoToBeMounted("test_%d" % pid)


def mirrorCommand(args):
    args1 = []
    args2 = []

    for arg in args:
        args1.append(arg.replace("__TESTDIR__", "test_%d" % pid))
        args2.append(arg.replace("__TESTDIR__", "test_%d_mirror" % pid))

    p = subprocess.run(args1)
    p.check_returncode()

    p = subprocess.run(args2)
    p.check_returncode()

    global mirrorTree
    if mirrorTree is not None and len(args2) == 2 and args2[0] == "mkdir":
        mirrorTree[0].append(os.path.normpath(args2[1]))
    else:
        mirrorTree = None


def verifyWithMirror():
    global argValgrind
    global argRepoPath
    global argEncryption
    global argPassphrase
    global valgrindProc
    global failOnError

    p = subprocess.run(["sync"])
    p.check_returncode()

    p = subprocess.run(["diff", "-r", "test_%d_mirror" % pid, "test_%d" % pid])
    p.check_returncode()

    remount()

    p = subprocess.run(["diff", "-r", "test_%d_mirror" % pid, "test_%d" % pid])
    p.check_returncode()


# Unmounts and mounts the repository again, without reading any file, so
# that the new bucse-mount process starts with an empty cache.
def remount():
    global argValgrind
    global argRepoPath
    global argPassphrase
    global valgrindProc
    global failOnError

    unmount()

    if argValgrind or failOnError:
        valgrindProc.communicate()
        if valgrindProc.returncode != 0:
            raise Exception("bucse-mount returned %d" % valgrindProc.returncode)

    p = subprocess.run(["../bucse-mount", "-p", argPassphrase, "-r", "%s/test_%d_repo" % (argRepoPath, pid), "test_%d" % pid])
    p.check_returncode()

    waitForRepoToBeMounted("test_%d" % pid)


def verifyFailOnError(printDebug = False):
    global valgrindProc
    global failOnError

    if not failOnError:
        return

    unmount()

    valgrindProc.communicate()
    if valgrindProc.returncode != 0:
        raise Exception("bucse-mount returned %d" % valgrindProc.returncode)

    with open("tmp/mount_%d.txt" % pid, "rb") as f:
        output = f.read().decode("utf-8", errors="replace")

    if printDebug:
        print(output)

    argsList = ["../bucse-mount", "-p", argPassphrase, "-r", "%s/test_%d_repo" % (argRepoPath, pid), "test_%d" % pid]
    if printDebug:
        argsList = argsList + ["-v", "4"]
    p = subprocess.run(argsList)
    p.check_returncode()

    if output.find("[error]") > -1:
        raise Exception("There were errors")


def testCleanup():
    global argRepoPath

    unmount()

    p = subprocess.run(["rm", "-rf", "test_%d_mirror" % pid])
    p.check_returncode()

    removeRepo()

    p = subprocess.run(["rm", "-rf", "test_%d" % pid])
    p.check_returncode()

    removeTmpFiles()
    for logFile in logFiles:
        os.remove("tmp/%s" % logFile)


# Removes the test repository. Also used by allTests.sh to clean up after a
# failed test.
def removeRepo():
    ssh = parseSshRepoPath()
    if ssh:
        sshArgs, repoPath = ssh
        p = subprocess.run(sshArgs + ["rm -rf %s" % remoteShellPath("%s/test_%d_repo" % (repoPath, pid))])
    else:
        p = subprocess.run(["rm", "-rf", "%s/test_%d_repo" % (argRepoPath, pid)])
    p.check_returncode()


# Lists of the directories and files in the mirror, used to pick random paths.
# Rebuilt after any mirror change except a plain mkdir, which is tracked.
mirrorTree = None

def scanMirror():
    dirs = []
    files = []
    stack = ["test_%d_mirror" % pid]
    while stack:
        path = stack.pop()
        dirs.append(path)
        with os.scandir(path) as it:
            for entry in it:
                if entry.is_dir(follow_symlinks=False):
                    stack.append(entry.path)
                else:
                    files.append(entry.path)
    return dirs, files


def getMirrorTree():
    global mirrorTree
    if mirrorTree is None:
        mirrorTree = scanMirror()
    return mirrorTree


def getRandomPathInMirror():
    return random.choice(getMirrorTree()[0])


def getRandomFileInMirror():
    return random.choice(getMirrorTree()[1])


randomCharacters = string.ascii_letters + string.digits
def getRandomFileName():
    return ''.join(random.choices(randomCharacters, k=random.randint(1, 64)))


def getRandomNewFileName():
    while True:
        candidate = getRandomPathInMirror() + "/" + getRandomFileName()
        if not os.path.exists(candidate):
            return candidate.replace("test_%d_mirror" % pid, "__TESTDIR__", 1)


def getRandomExistingDirName():
            return getRandomPathInMirror().replace("test_%d_mirror" % pid, "__TESTDIR__", 1)


def getRandomExistingFileName():
            return getRandomFileInMirror().replace("test_%d_mirror" % pid, "__TESTDIR__", 1)


# Writes a file of random bytes to tmp/ and returns its name.
def writeRandomTmpFile(fileSize):
    fileName = getRandomFileName()
    while os.path.exists("tmp/" + fileName):
        fileName = getRandomFileName()

    with open("tmp/" + fileName, "wb") as f:
        remaining = fileSize
        while remaining > 0:
            chunk = min(remaining, 1024 * 1024)
            f.write(random.randbytes(chunk))
            remaining -= chunk

    tmpFiles.append(fileName)
    return fileName


def makeRandomTmpFile(size = 1024 * 1024, randomUpTo = True):
    if randomUpTo:
        size = random.randint(0, size)
    return writeRandomTmpFile(size)


def makeRandomTmpFileKBytes(kBytesSize = 1024, randomUpTo = True):
    if randomUpTo:
        kBytesSize = random.randint(0, kBytesSize)
    return writeRandomTmpFile(kBytesSize * 1024)


def mirrorOpen(fileName):
    fileName1 = []
    fileName2 = []

    fileName1 = fileName.replace("__TESTDIR__", "test_%d" % pid)
    fileName2 = fileName.replace("__TESTDIR__", "test_%d_mirror" % pid)
    
    fd1 = os.open(fileName1, flags=os.O_RDWR)
    fd2 = os.open(fileName2, flags=os.O_RDWR)

    return fd1, fd2


def mirrorCreate(fileName):
    fileName1 = []
    fileName2 = []

    fileName1 = fileName.replace("__TESTDIR__", "test_%d" % pid)
    fileName2 = fileName.replace("__TESTDIR__", "test_%d_mirror" % pid)
    
    fd1 = os.open(fileName1, flags=os.O_CREAT|os.O_RDWR)
    fd2 = os.open(fileName2, flags=os.O_CREAT|os.O_RDWR)

    global mirrorTree
    mirrorTree = None

    return fd1, fd2


def mirrorRandomOp(fileName, fd, fdMirror):
    fileSize = os.stat(fdMirror).st_size

    op = random.choice(["read", "write"])
    if op == "read":
        begin = random.randint(0, fileSize-1)
        end = random.randint(begin, fileSize-1)
        opsize = end - begin
        if opsize == 0:
            return

        os.lseek(fd, begin, os.SEEK_SET)
        os.lseek(fdMirror, begin, os.SEEK_SET)

        buf1 = os.read(fd, opsize)
        buf2 = os.read(fdMirror, opsize)

        if buf1 != buf2:
            raise Exception("Read operation did not end with the same result. Filename %s, begin %d, size %d"%(fileName, begin, opsize))

    elif op == "write":
        begin = random.randint(0, fileSize-1)
        end = random.randint(begin, fileSize-1 + 1024*128)
        opsize = end - begin
        if opsize == 0:
            return

        os.lseek(fd, begin, os.SEEK_SET)
        os.lseek(fdMirror, begin, os.SEEK_SET)

        buf = random.randbytes(opsize)
        os.write(fd, buf)
        os.write(fdMirror, buf)

    print([op, begin, end, fileSize])


def mirrorOp(fileName, fd, fdMirror, op, size, offset):
    if op == "read":
        os.lseek(fd, offset, os.SEEK_SET)
        os.lseek(fdMirror, offset, os.SEEK_SET)

        buf1 = os.read(fd, size)
        buf2 = os.read(fdMirror, size)

        if buf1 != buf2:
            raise Exception("Read operation did not end with the same result. Filename %s, offset %d, size %d"%(fileName, offset, size))

    elif op == "write":
        os.lseek(fd, offset, os.SEEK_SET)
        os.lseek(fdMirror, offset, os.SEEK_SET)

        buf = random.randbytes(size)
        os.write(fd, buf)
        os.write(fdMirror, buf)

    elif op == "flush":
        os.fsync(fd)
        os.fsync(fdMirror)

    elif op == "truncate":
        os.ftruncate(fd, size)
        os.ftruncate(fdMirror, size)

    print([op, offset, size])


def mirrorClose(filename, fd, fdMirror):
    os.close(fd)
    os.close(fdMirror)


# Access to the files of the test repository, for both local and ssh
# destinations.

# Returns (ssh command prefix, path on the remote host) for an ssh repository
# path, or None for a local one.
def parseSshRepoPath():
    r = re.match(r'ssh://(.*?)(:(\d+))?/(.*)$', argRepoPath)
    if not r:
        return None
    hostname = r.groups()[0]
    port = r.groups()[2]
    sshArgs = ["ssh"]
    if port:
        sshArgs = sshArgs + ["-p", port]
    return sshArgs + [hostname], r.groups()[3]


# Quotes a remote path for the remote shell, leaving a leading ~/ unquoted so
# that it is expanded to the home directory.
def remoteShellPath(path):
    if path.startswith("~/"):
        return "~/" + shlex.quote(path[2:])
    return shlex.quote(path)


# Writes a file of the test repository. The data is written under a temporary
# name outside actions/ first and then renamed, so bucse-mount never sees a
# half-written file (copyActionPartially() writes a partial one on purpose).
def writeRepoFile(relPath, data):
    tmpName = "tmp_%d_%s" % (pid, os.path.basename(relPath))
    ssh = parseSshRepoPath()
    if ssh:
        sshArgs, repoPath = ssh
        repo = "%s/test_%d_repo" % (repoPath, pid)
        tmpPath = remoteShellPath("%s/%s" % (repo, tmpName))
        remotePath = remoteShellPath("%s/%s" % (repo, relPath))
        subprocess.run(sshArgs + ["cat > %s && mv %s %s" % (tmpPath, tmpPath, remotePath)], input=data, check=True)
    else:
        repo = "%s/test_%d_repo" % (argRepoPath, pid)
        with open("%s/%s" % (repo, tmpName), "wb") as f:
            f.write(data)
        os.rename("%s/%s" % (repo, tmpName), "%s/%s" % (repo, relPath))


def deleteRepoFile(relPath):
    ssh = parseSshRepoPath()
    if ssh:
        sshArgs, repoPath = ssh
        remotePath = remoteShellPath("%s/test_%d_repo/%s" % (repoPath, pid, relPath))
        subprocess.run(sshArgs + ["rm -f %s" % remotePath], check=True)
    else:
        path = "%s/test_%d_repo/%s" % (argRepoPath, pid, relPath)
        if os.path.exists(path):
            os.remove(path)


# Encrypts data the way the repository's encryption backend does. The aes
# backend is compatible with `openssl enc`, see encryption/encr_aes.c.
def encryptForRepo(data):
    if argEncryption == "none":
        return data
    if argEncryption == "aes":
        p = subprocess.run(["openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-md", "sha1", "-iter", "1",
                            "-pass", "env:BUCSE_TEST_PASSPHRASE"],
                           input=data, capture_output=True, check=True,
                           env=dict(os.environ, BUCSE_TEST_PASSPHRASE=argPassphrase))
        return p.stdout
    raise Exception("Unsupported encryption: %s" % argEncryption)


# Action files in tests/data are plain JSON. These functions encrypt them with
# the repository's encryption backend and put them into the repository.

def readEncryptedAction(actionFile):
    with open(actionFile, "rb") as f:
        return encryptForRepo(f.read())


def copyAction(actionFile):
    writeRepoFile("actions/%s" % os.path.basename(actionFile), readEncryptedAction(actionFile))


def copyActions(actionsDir):
    for name in sorted(os.listdir(actionsDir)):
        copyAction(os.path.join(actionsDir, name))


# Writes only the first part of an action file, as if its upload was interrupted.
def copyActionPartially(actionFile, fraction = 0.5):
    data = readEncryptedAction(actionFile)
    writeRepoFile("actions/%s" % os.path.basename(actionFile), data[:int(len(data) * fraction)])


# Packs (encrypted) action files into one tar file in the repository, like a
# maintenance tool packing action files does.
def copyActionsAsTar(actionsDir, tarName):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name in sorted(os.listdir(actionsDir)):
            data = readEncryptedAction(os.path.join(actionsDir, name))
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = int(time.time())
            tar.addfile(info, io.BytesIO(data))
    writeRepoFile("actions/%s" % tarName, buf.getvalue())


def deleteAction(actionName):
    deleteRepoFile("actions/%s" % actionName)


# Waits until all the given paths (relative to the mount) exist in the
# mounted repository.
def waitForFiles(paths, timeout = 30):
    deadline = time.time() + timeout
    while True:
        missing = [p for p in paths if not os.path.exists("test_%d/%s" % (pid, p))]
        if not missing:
            return
        if time.time() > deadline:
            raise Exception("Timeout waiting for %s" % missing)
        time.sleep(0.2)


markerCount = 0

# Waits until bucse-mount has handled everything put into the repository so
# far. Adds a marker action (a new directory) and waits until the directory
# shows up: bucse-mount reads new action files in batches, so by then it has
# also read the ones written before the marker.
def waitForActionsHandled(timeout = 30):
    global markerCount
    markerCount += 1
    name = "marker_%d" % markerCount
    action = [{
        "time": int(time.time() * 1000000),
        "action": "addDirectory",
        "path": name,
        "content": [],
        "size": 0,
        "blockSize": 0,
    }]
    data = json.dumps(action).encode("utf-8")
    writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(), encryptForRepo(data))
    waitForFiles([name], timeout)

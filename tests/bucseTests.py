import os
import subprocess
import time
import random
import string
import argparse
import re

pid = os.getpid()
tmpFiles = []

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


def waitForRepoToBeMounted(mountDir):
    while True:
        mountProcess = subprocess.run(['mount'], check=True, capture_output=True)
        grepProcess = subprocess.run(['grep', os.path.realpath(mountDir)],
                              input=mountProcess.stdout, capture_output=True)
        if grepProcess.returncode == 0:
            return
        time.sleep(1)


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

    p = subprocess.run(["../bucse-init", "-e", argEncryption, "-p", argPassphrase, "%s/test_%d_repo" % (argRepoPath, pid)])
    p.check_returncode()

    argsList = ["../bucse-mount", "-p", argPassphrase, "-r", "%s/test_%d_repo" % (argRepoPath, pid), "test_%d" % pid]

    if failOnError:
        argsList += ["-f", "-v 4"]

    if argDebug:
        print(" ".join(argsList + ["-f -v 4"]))
        input()
    elif argValgrind:
        argsList = ["valgrind", "--log-file=tmp/valgrind_%d.txt" % pid, "--error-exitcode=-1", "--leak-check=full", "--show-leak-kinds=all", "--errors-for-leak-kinds=all", "--exit-on-first-error=yes"] + argsList + ["-f"]
        tmpFiles.append("valgrind_%d.txt" % pid)
        valgrindProc = subprocess.Popen(argsList, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif failOnError:
        valgrindProc = subprocess.Popen(argsList, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    else:
        p = subprocess.run(argsList)
        p.check_returncode()
    
    waitForRepoToBeMounted("%s/test_%d" % (argRepoPath, pid))


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

    p = subprocess.run(["umount", "test_%d" % pid])
    p.check_returncode()

    if argValgrind or failOnError:
        valgrindProc.communicate()
        if valgrindProc.returncode != 0:
            raise Exception("bucse-mount returned %d" % valgrindProc.returncode)

    p = subprocess.run(["../bucse-mount", "-p", argPassphrase, "-r", "%s/test_%d_repo" % (argRepoPath, pid), "test_%d" % pid])
    p.check_returncode()

    waitForRepoToBeMounted("test_%d" % pid)
    time.sleep(5)

    p = subprocess.run(["diff", "-r", "test_%d_mirror" % pid, "test_%d" % pid])
    p.check_returncode()


def verifyFailOnError():
    global valgrindProc
    global failOnError

    if not failOnError:
        return

    p = subprocess.run(["umount", "test_%d" % pid])
    p.check_returncode()

    outputBytes, errBytes = valgrindProc.communicate()
    if valgrindProc.returncode != 0:
        raise Exception("bucse-mount returned %d" % valgrindProc.returncode)

    p = subprocess.run(["../bucse-mount", "-p", argPassphrase, "-r", "%s/test_%d_repo" % (argRepoPath, pid), "test_%d" % pid])
    p.check_returncode()

    if outputBytes.decode("utf-8").find("[error]") > -1:
        raise Exception("There were errors");


def testCleanup():
    global argRepoPath

    time.sleep(1)

    p = subprocess.run(["umount", "test_%d" % pid])
    p.check_returncode()

    p = subprocess.run(["rm", "-rf", "test_%d_mirror" % pid])
    p.check_returncode()

    r = re.match(r'ssh://(.*?)(:(\d+))?/(.*)$', argRepoPath)
    if r:
        hostname = r.groups()[0]
        port = r.groups()[2]
        repoPath = r.groups()[3]
        argsList = ["ssh"]
        if port:
            argsList = argsList + ["-p", port]
        argsList = argsList + [hostname, "rm", "-rf", "%s/test_%d_repo"%(repoPath, pid)]
        p = subprocess.run(argsList)
    else:
        p = subprocess.run(["rm", "-rf", "%s/test_%d_repo" % (argRepoPath, pid)])
    p.check_returncode()

    p = subprocess.run(["rm", "-rf", "test_%d" % pid])
    p.check_returncode()

    for tmpFile in tmpFiles:
        p = subprocess.run(["rm", "-rf", "tmp/%s" % tmpFile])
        p.check_returncode()


def getRandomPathInMirror():
    p = subprocess.run(["find", "test_%d_mirror" % pid, "-type", "d"], capture_output = True)
    p.check_returncode()

    dirs = p.stdout.decode("UTF-8").split("\n")[:-1]
    return random.choice(dirs)


def getRandomFileInMirror():
    p = subprocess.run(["find", "test_%d_mirror" % pid, "-type", "f"], capture_output = True)
    p.check_returncode()

    dirs = p.stdout.decode("UTF-8").split("\n")[:-1]
    return random.choice(dirs)


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


def makeRandomTmpFile(size = 1024 * 1024, randomUpTo = True):
    if randomUpTo:
        fileSize = random.randint(0, size)
    else:
        fileSize = size
    fileName = ""
    while os.path.exists("tmp/" + fileName):
        fileName = getRandomFileName()

    fileName = getRandomFileName()

    p = subprocess.run(["dd", "bs=1", "count=%d"%fileSize, "if=/dev/random", "of=tmp/%s"%fileName, "status=none"])
    p.check_returncode()


    tmpFiles.append(fileName)
    return fileName


def makeRandomTmpFileKBytes(kBytesSize = 1024, randomUpTo = True):
    if randomUpTo:
        fileSize = random.randint(0, kBytesSize)
    else:
        fileSize = kBytesSize
    fileName = ""
    while os.path.exists("tmp/" + fileName):
        fileName = getRandomFileName()

    fileName = getRandomFileName()

    p = subprocess.run(["dd", "bs=1024", "count=%d"%fileSize, "if=/dev/random", "of=tmp/%s"%fileName, "status=none"])
    p.check_returncode()


    tmpFiles.append(fileName)
    return fileName


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

def copyActions(actionsDir):
    p = subprocess.run(["cp -f %s/* test_%d_repo/actions/" % (actionsDir, pid)], shell=True)
    p.check_returncode()

import os
import subprocess
import time
import random
import string
import argparse

pid = os.getpid()
tmpFiles = []

argDebug = False
argValgrind = False
valgrindProc = None

def parseArgs():
    global argDebug
    global argValgrind
    parser = argparse.ArgumentParser()
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--debug",
        help="Do not run bucse-mount, just print the command that is intended to be run so it can be started with a gdb manually, then wait for newline on stdin to continue execution.",
        action="store_true")
    group.add_argument("--valgrind",
        help="Use valgrind.",
        action="store_true")
    args = parser.parse_args()
    if args.debug:
        argDebug = True
    if args.valgrind:
        argValgrind = True

def downloadTmp(url, filename, shasum):
    p = subprocess.run(["wget", "-O", "tmp/%s"%filename, url])
    p.check_returncode()

    p = subprocess.run(["shasum", "-c"], input=bytes("%s *tmp/%s\n"%(shasum, filename), 'UTF-8'))
    p.check_returncode()

    tmpFiles.append(filename)


def mountDirs():
    global argDebug
    global argValgrind
    global valgrindProc

    p = subprocess.run(["mkdir", "test_%d_mirror" % pid])
    p.check_returncode()

    p = subprocess.run(["mkdir", "test_%d" % pid])
    p.check_returncode()

    p = subprocess.run(["../bucse-init", "test_%d_repo" % pid])
    p.check_returncode()

    if argDebug:
        print(" ".join(["../bucse-mount", "-f -v 4", "-r", "test_%d_repo" % pid, "test_%d" % pid]))
        input()
    else:
        argsList = ["../bucse-mount", "-r", "test_%d_repo" % pid, "test_%d" % pid]
        if argValgrind:
            argsList = ["valgrind", "--log-file=tmp/valgrind_%d.txt" % pid, "--error-exitcode=-1", "--leak-check=full", "--show-leak-kinds=all", "--errors-for-leak-kinds=all", "--exit-on-first-error=yes"] + argsList + ["-f"]
            tmpFiles.append("valgrind_%d.txt" % pid)
            valgrindProc = subprocess.Popen(argsList, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            p = subprocess.run(argsList)
            p.check_returncode()
        time.sleep(5)


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
    global valgrindProc

    p = subprocess.run(["sync"])
    p.check_returncode()

    p = subprocess.run(["diff", "-r", "test_%d_mirror" % pid, "test_%d" % pid])
    p.check_returncode()

    p = subprocess.run(["umount", "test_%d" % pid])
    p.check_returncode()

    if argValgrind:
        valgrindProc.communicate()
        if valgrindProc.returncode != 0:
            raise Exception("bucse-mount returned %d" % valgrindProc.returncode)

    p = subprocess.run(["../bucse-mount", "-r", "test_%d_repo" % pid, "test_%d" % pid])
    p.check_returncode()

    time.sleep(5)

    p = subprocess.run(["diff", "-r", "test_%d_mirror" % pid, "test_%d" % pid])
    p.check_returncode()


def testCleanup():
    time.sleep(1)

    p = subprocess.run(["umount", "test_%d" % pid])
    p.check_returncode()

    p = subprocess.run(["rm", "-rf", "test_%d_mirror" % pid])
    p.check_returncode()

    p = subprocess.run(["rm", "-rf", "test_%d_repo" % pid])
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

def makeRandomTmpFile():
    fileSize = random.randint(0, 1024*1024)
    fileName = ""
    while os.path.exists("tmp/" + fileName):
        fileName = getRandomFileName()

    fileName = getRandomFileName()

    p = subprocess.run(["dd", "bs=1", "count=%d"%fileSize, "if=/dev/random", "of=tmp/%s"%fileName, "status=none"])
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

    print([op, offset, size])


def mirrorClose(filename, fd, fdMirror):
    os.close(fd)
    os.close(fdMirror)

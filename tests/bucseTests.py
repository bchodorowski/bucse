import os
import subprocess
import time
import random
import string

pid = os.getpid()
tmpFiles = []

def downloadTmp(url, filename, shasum):
    p = subprocess.run(["wget", "-O", "tmp/%s"%filename, url])
    p.check_returncode()

    p = subprocess.run(["shasum", "-c"], input=bytes("%s *tmp/%s\n"%(shasum, filename), 'UTF-8'))
    p.check_returncode()

    tmpFiles.append(filename)


def mountDirs():
    p = subprocess.run(["mkdir", "test_%d_mirror" % pid])
    p.check_returncode()

    p = subprocess.run(["mkdir", "test_%d" % pid])
    p.check_returncode()

    p = subprocess.run(["../bucse-init", "test_%d_repo" % pid])
    p.check_returncode()

    p = subprocess.run(["../bucse-mount", "-r", "test_%d_repo" % pid, "test_%d" % pid])
    p.check_returncode()


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
    p = subprocess.run(["sync"])
    p.check_returncode()

    p = subprocess.run(["diff", "-r", "test_%d_mirror" % pid, "test_%d" % pid])
    p.check_returncode()

    p = subprocess.run(["umount", "test_%d" % pid])
    p.check_returncode()

    p = subprocess.run(["../bucse-mount", "-r", "test_%d_repo" % pid, "test_%d" % pid])
    p.check_returncode()

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


randomCharacters = string.ascii_letters + string.digits
def getRandomFileName():
    return ''.join(random.choices(randomCharacters, k=random.randint(1, 64)))


def getRandomNewFileName():
    while True:
        candidate = getRandomPathInMirror() + "/" + getRandomFileName()
        if not os.path.exists(candidate):
            return candidate.replace("test_%d_mirror" % pid, "__TESTDIR__", 1)

def getRandomExistingFileName():
            return getRandomPathInMirror().replace("test_%d_mirror" % pid, "__TESTDIR__", 1)

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

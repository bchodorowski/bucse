#!/bin/python3

import bucseTests


bucseTests.parseArgs()


bucseTests.mountDirs()

for _ in range(128 * 10):
    bucseTests.mirrorCommand(["mkdir", bucseTests.getRandomNewFileName()])
for _ in range(10 * 10):
    fileName = bucseTests.makeRandomTmpFile()
    targetDir = bucseTests.getRandomExistingDirName()
    bucseTests.mirrorCommand(["cp", "tmp/%s"%fileName, "%s/"%targetDir])

for _ in range(5 * 10):
    fileName = bucseTests.getRandomExistingFileName()
    print(fileName)
    fd, fdMirror = bucseTests.mirrorOpen(fileName)
    print([fd, fdMirror])
    for _ in range(20):
        bucseTests.mirrorRandomOp(fileName, fd, fdMirror)
    bucseTests.mirrorClose(fileName, fd, fdMirror)

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

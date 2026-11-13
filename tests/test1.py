#!/bin/python3

import bucseTests


bucseTests.parseArgs()


bigFileName = bucseTests.makeRandomTmpFileKBytes(16 * 1024, False)

bucseTests.mountDirs()

bucseTests.mirrorCommand(["cp", "tmp/%s"%bigFileName, "__TESTDIR__/"])

for _ in range(128):
    bucseTests.mirrorCommand(["mkdir", bucseTests.getRandomNewFileName()])
for _ in range(5):
    fileName = bucseTests.makeRandomTmpFile()
    targetDir = bucseTests.getRandomExistingDirName()
    bucseTests.mirrorCommand(["cp", "tmp/%s"%fileName, "%s/"%targetDir])

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

#!/bin/python3

import bucseTests


bucseTests.parseArgs()


# the file in tmp/ and the mirror, and the repository (a bit more than the
# file: its blocks are rewritten when they are resized)
bucseTests.requireFreeSpace(2 * 5200000 * 1024, 1.3 * 5200000 * 1024)

fileName = bucseTests.makeRandomTmpFileKBytes(5200000, False)

bucseTests.mountDirs()

bucseTests.mirrorCommand(["cp", "tmp/%s"%fileName, "__TESTDIR__/"])

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

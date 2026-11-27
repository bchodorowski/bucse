#!/bin/python3

import bucseTests


bucseTests.parseArgs()


# the file in tmp/ and the mirror, and the repository (a bit more than the
# file: its blocks are rewritten when they are resized)
bucseTests.requireFreeSpace(2 * 5200000 * 1024, 1.3 * 5200000 * 1024)

fileName = bucseTests.makeRandomTmpFileKBytes(5200000, False)

bucseTests.mountDirs()

bucseTests.mirrorCommand(["dd", "bs=512", "if=tmp/%s"%fileName, "of=__TESTDIR__/%s"%fileName])

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

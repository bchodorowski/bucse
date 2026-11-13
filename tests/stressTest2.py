#!/bin/python3

import bucseTests


bucseTests.parseArgs()


fileName = bucseTests.makeRandomTmpFileKBytes(5200000, False)

bucseTests.mountDirs()

bucseTests.mirrorCommand(["dd", "bs=512", "if=tmp/%s"%fileName, "of=__TESTDIR__/%s"%fileName])

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

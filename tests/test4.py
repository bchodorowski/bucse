#!/bin/python3

import bucseTests


bucseTests.parseArgs()


fileName = bucseTests.makeRandomTmpFileKBytes(20000, False)

bucseTests.mountDirs()

bucseTests.mirrorCommand(["cp", "tmp/%s"%fileName, "__TESTDIR__/"])

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

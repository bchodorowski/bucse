#!/bin/python3

import bucseTests


bucseTests.parseArgs()


bucseTests.mountDirs()

fileName = "__TESTDIR__/testfile.bin"

fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 256*1024)
bucseTests.mirrorClose(fileName, fd, fdMirror)

bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/foo"])

bucseTests.mirrorCommand(["mv", fileName, "__TESTDIR__/foo"])


bucseTests.verifyWithMirror()
bucseTests.testCleanup()

#!/bin/python3

import bucseTests


bucseTests.parseArgs()


bucseTests.mountDirs()

fileName = "__TESTDIR__/testfile.bin"

fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 256*1024)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fd, fdMirror = bucseTests.mirrorOpen(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "truncate", (192)*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)


bucseTests.verifyWithMirror()
bucseTests.testCleanup()

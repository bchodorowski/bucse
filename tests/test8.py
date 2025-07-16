#!/bin/python3

import bucseTests


bucseTests.parseArgs()


bucseTests.mountDirs()

bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d1"])

bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d2"])

bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d3"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d3/dd1"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d3/dd1/ddd1"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d3/dd1/ddd2"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d3/dd2"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d3/dd3"])

bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d4"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d4/dd1"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d4/dd1/ddd1"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d4/dd1/ddd2"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d4/dd2"])
bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d4/dd3"])

bucseTests.mirrorCommand(["mkdir", "__TESTDIR__/d5"])

fileName = "__TESTDIR__/f1.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/f2.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/f3.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/f4.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/f5.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d1/ff1.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d1/ff2.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d3/ff1.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d3/ff2.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d3/dd1/fff1.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d4/ff1.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d4/ff2.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

fileName = "__TESTDIR__/d4/dd1/fff1.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 128*1024, 0)
bucseTests.mirrorClose(fileName, fd, fdMirror)

# rename in the root
bucseTests.mirrorCommand(["mv", "__TESTDIR__/f1.bin", "__TESTDIR__/f4.bin"])

# rename in the root, overwrite another file
bucseTests.mirrorCommand(["mv", "__TESTDIR__/f2.bin", "__TESTDIR__/f3.bin"])

# move to a directory
bucseTests.mirrorCommand(["mv", "__TESTDIR__/f4.bin", "__TESTDIR__/d1"])

# move to a directory, overwrite another file
bucseTests.mirrorCommand(["mv", "__TESTDIR__/f5.bin", "__TESTDIR__/d1/ff1.bin"])

# move from a directory to root
bucseTests.mirrorCommand(["mv", "__TESTDIR__/d1/ff2.bin", "__TESTDIR__"])

# rename a directory
bucseTests.mirrorCommand(["mv", "__TESTDIR__/d3", "__TESTDIR__/d6"])

# rename a directory to an existing one
bucseTests.mirrorCommand(["mv", "__TESTDIR__/d4", "__TESTDIR__/d5"])

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

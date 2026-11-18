#!/bin/python3

# Files bigger than 2 GB and 4 GB: offsets and block positions past 2^31 and
# 2^32 must not overflow (flush, truncate). Only small writes are made, at
# high offsets, but bucse stores the zeroes before them: the repository grows
# by about 6.5 GB (the mirror files are sparse).

import bucseTests


bucseTests.parseArgs()

bucseTests.mountDirs()

GB = 1024 * 1024 * 1024

# writes across the 2 GB boundary and past 4 GB, flushed at once
bigFile = bucseTests.getRandomNewFileName()
fd, fdMirror = bucseTests.mirrorCreate(bigFile)
bucseTests.mirrorOp(bigFile, fd, fdMirror, "write", 4096, 0)
bucseTests.mirrorOp(bigFile, fd, fdMirror, "write", 4096, 2 * GB - 100)
bucseTests.mirrorOp(bigFile, fd, fdMirror, "write", 4096, 4 * GB + 50)
bucseTests.mirrorClose(bigFile, fd, fdMirror)

# a small file (4 MB blocks) extended past 2 GB: its blocks are resized
resizedFile = bucseTests.getRandomNewFileName()
fd, fdMirror = bucseTests.mirrorCreate(resizedFile)
bucseTests.mirrorOp(resizedFile, fd, fdMirror, "write", 20 * 1024 * 1024, 0)
bucseTests.mirrorClose(resizedFile, fd, fdMirror)
fd, fdMirror = bucseTests.mirrorOpen(resizedFile)
bucseTests.mirrorOp(resizedFile, fd, fdMirror, "write", 4096, 2 * GB + 50)
bucseTests.mirrorClose(resizedFile, fd, fdMirror)

# shrink the big file to a size past 2 GB, write near the new end, and extend
# it with truncate
fd, fdMirror = bucseTests.mirrorOpen(bigFile)
bucseTests.mirrorOp(bigFile, fd, fdMirror, "truncate", 3 * GB + 123, 0)
bucseTests.mirrorOp(bigFile, fd, fdMirror, "write", 10, 3 * GB - 10)
bucseTests.mirrorOp(bigFile, fd, fdMirror, "truncate", 3 * GB + 1123, 0)
bucseTests.mirrorClose(bigFile, fd, fdMirror)

# read everything back from the repository
bucseTests.remount()

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

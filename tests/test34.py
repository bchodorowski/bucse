#!/bin/python3

# An extending truncate is a pending truncate, like a shrinking one: the
# blocks past the old size are written as zeroes when the file is flushed,
# one block at a time. It used to be a zero-filled pending write of the whole
# gap, allocated at once, so e.g. truncate -s 3G failed with ENOMEM on a
# machine with less RAM + swap than that.

import bucseTests
import os

MB = 1024 * 1024


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()


def vmPeak():
    with open("/proc/%d/status" % bucseTests.valgrindProc.pid) as f:
        for line in f:
            if line.startswith("VmPeak:"):
                return int(line.split()[1]) * 1024
    raise Exception("VmPeak not found")


def op(fileName, fd, fdMirror, opName, size, offset = 0):
    bucseTests.mirrorOp(fileName, fd, fdMirror, opName, size, offset)


# an empty file extended
fileName = "__TESTDIR__/empty.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
op(fileName, fd, fdMirror, "truncate", 3000)
bucseTests.mirrorClose(fileName, fd, fdMirror)

# extended within its last block, then past it
fileName = "__TESTDIR__/lastBlock.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
op(fileName, fd, fdMirror, "write", 1000)
bucseTests.mirrorClose(fileName, fd, fdMirror)
fd, fdMirror = bucseTests.mirrorOpen(fileName)
op(fileName, fd, fdMirror, "truncate", 1020)
bucseTests.mirrorClose(fileName, fd, fdMirror)
fd, fdMirror = bucseTests.mirrorOpen(fileName)
op(fileName, fd, fdMirror, "truncate", 2100)
bucseTests.mirrorClose(fileName, fd, fdMirror)

# shrunk, then extended while open: the cut off bytes must read as zeroes
fileName = "__TESTDIR__/shrinkExtend.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
op(fileName, fd, fdMirror, "write", 10000)
bucseTests.mirrorClose(fileName, fd, fdMirror)
fd, fdMirror = bucseTests.mirrorOpen(fileName)
op(fileName, fd, fdMirror, "truncate", 100)
op(fileName, fd, fdMirror, "truncate", 5000)
op(fileName, fd, fdMirror, "read", 5000)
bucseTests.mirrorClose(fileName, fd, fdMirror)

# extended, then written inside the gap and past its end
fileName = "__TESTDIR__/extendWrite.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
op(fileName, fd, fdMirror, "write", 700)
bucseTests.mirrorClose(fileName, fd, fdMirror)
fd, fdMirror = bucseTests.mirrorOpen(fileName)
op(fileName, fd, fdMirror, "truncate", 20000)
op(fileName, fd, fdMirror, "write", 300, 5000)
op(fileName, fd, fdMirror, "write", 300, 25000)
bucseTests.mirrorClose(fileName, fd, fdMirror)

# a file with small blocks extended to many more blocks: the content is
# resized to bigger blocks
fileName = "__TESTDIR__/resize.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
op(fileName, fd, fdMirror, "write", 1500)
bucseTests.mirrorClose(fileName, fd, fdMirror)
fd, fdMirror = bucseTests.mirrorOpen(fileName)
op(fileName, fd, fdMirror, "truncate", 300 * 1024 + 7)
bucseTests.mirrorClose(fileName, fd, fdMirror)

# the memory: nothing is allocated for the gap until the flush, and the flush
# needs only a few buffers of one block (here 8 MB)
fileName = "__TESTDIR__/big.bin"
fd, fdMirror = bucseTests.mirrorCreate(fileName)
before = vmPeak()
op(fileName, fd, fdMirror, "truncate", 32 * MB)
afterTruncate = vmPeak()
assert afterTruncate - before < 8 * MB, \
    "VmPeak grew by %d MB after the truncate" % ((afterTruncate - before) // MB)
assert os.fstat(fd).st_size == 32 * MB
bucseTests.mirrorClose(fileName, fd, fdMirror)

bucseTests.verifyWithMirror()
bucseTests.verifyFailOnError()
bucseTests.testCleanup()

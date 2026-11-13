#!/bin/python3

# Shrink files so that their last block becomes shorter, then write to that
# block, leaving a gap after the new end that must read as zeroes (not as the
# old data). Done twice: with blocks cached, and after a remount, when the
# blocks are read from the repository.

import bucseTests


bucseTests.parseArgs()


bucseTests.mountDirs()

# small files have 512-byte blocks, the bigger one 8 KiB blocks
cases = [(1000, 600), (1000, 513), (3000, 1100), (40000, 20000)]


def createAndShrink(readBeforeShrink):
    fileNames = []
    for size, newSize in cases:
        fileName = bucseTests.getRandomNewFileName()
        fileNames.append(fileName)
        fd, fdMirror = bucseTests.mirrorCreate(fileName)
        bucseTests.mirrorOp(fileName, fd, fdMirror, "write", size, 0)
        bucseTests.mirrorClose(fileName, fd, fdMirror)

        fd, fdMirror = bucseTests.mirrorOpen(fileName)
        if readBeforeShrink:
            # puts the blocks into the cache
            bucseTests.mirrorOp(fileName, fd, fdMirror, "read", size, 0)
        bucseTests.mirrorOp(fileName, fd, fdMirror, "truncate", newSize, 0)
        bucseTests.mirrorClose(fileName, fd, fdMirror)
    return fileNames


def writeAfterShrink(fileNames):
    for (size, newSize), fileName in zip(cases, fileNames):
        fd, fdMirror = bucseTests.mirrorOpen(fileName)
        # write within the shortened block, with a gap after the new end
        bucseTests.mirrorOp(fileName, fd, fdMirror, "write", 10, newSize + 50)
        # and extend the file past the old end
        bucseTests.mirrorOp(fileName, fd, fdMirror, "truncate", size + 100, 0)
        bucseTests.mirrorClose(fileName, fd, fdMirror)


cachedFiles = createAndShrink(True)
writeAfterShrink(cachedFiles)

uncachedFiles = createAndShrink(False)
# remount without reading anything, so the blocks are not cached
bucseTests.remount()
writeAfterShrink(uncachedFiles)

bucseTests.verifyWithMirror()
bucseTests.testCleanup()

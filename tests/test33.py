#!/bin/python3

# The size, blockSize and content of an addFile or editFile action come from
# the (untrusted) server. An action whose block size isn't 0 or a power of 2
# from 512 B to 128 MB, whose size is negative, or whose content has fewer
# blocks than its size needs, is rejected. bucse-mount used to divide by a
# zero block size (and crash), malloc buffers of a huge or negative block
# size, and read past the end of a too short content.

import bucseTests
import errno
import hashlib
import json
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()


def mountPath(path):
    return "test_%d/%s" % (bucseTests.pid, path)


actionTime = int(time.time() * 1000000)


def writeAction(actionType, path, content, size, blockSize):
    global actionTime
    actionTime += 1
    data = json.dumps([{
        "time": actionTime,
        "action": actionType,
        "path": path,
        "content": content,
        "size": size,
        "blockSize": blockSize,
    }]).encode("utf-8")
    bucseTests.writeRepoFile("actions/%s" % hashlib.sha1(data).hexdigest(),
        bucseTests.encryptForRepo(data))


def writeBlock(blockData):
    blockName = hashlib.sha1(blockData).hexdigest()
    bucseTests.writeRepoFile("storage/%s" % blockName, bucseTests.encryptForRepo(blockData))
    return blockName


blocks = [os.urandom(512), os.urandom(512), os.urandom(100)]
names = [writeBlock(b) for b in blocks]
fileData = b"".join(blocks)

# rejected: (path, content, size, blockSize)
invalid = [
    ("zero_block_size", names, len(fileData), 0),
    ("negative_block_size", names, len(fileData), -512),
    ("not_power_of_2", names, len(fileData), 1000),
    ("too_small", names, len(fileData), 256),
    ("too_large", names, len(fileData), 256 * 1024 * 1024),
    ("huge", names, len(fileData), 2 ** 40),
    ("negative_size", names, -1, 512),
    ("content_too_short", names[:2], len(fileData), 512),
    ("content_empty", [], 1, 512),
]
for path, content, size, blockSize in invalid:
    writeAction("addFile", path, content, size, blockSize)

# accepted
writeAction("addFile", "empty", [], 0, 0)
writeAction("addFile", "empty_with_block_size", [], 0, 512)
writeAction("addFile", "ok", names, len(fileData), 512)
# a longer content is fine, the blocks past the size are never read
writeAction("addFile", "longer_content", names + [names[0]], len(fileData), 512)
writeAction("addFile", "max_block_size", [names[0]], 512, 128 * 1024 * 1024)

# an invalid editFile is rejected too, the file keeps its content
writeAction("addFile", "edited", names, len(fileData), 512)
writeAction("editFile", "edited", names, len(fileData), 0)
writeAction("editFile", "edited", names[:1], len(fileData), 512)

bucseTests.waitForActionsHandled()

for path, content, size, blockSize in invalid:
    assert not os.path.exists(mountPath(path)), "%s exists" % path

def readFile(path):
    with open(mountPath(path), "rb") as f:
        return f.read()

assert readFile("empty") == b""
assert readFile("empty_with_block_size") == b""
assert readFile("ok") == fileData
assert readFile("longer_content") == fileData
assert readFile("max_block_size") == blocks[0]
assert readFile("edited") == fileData

bucseTests.verifyFailOnError(True, allowedErrors = [
    "parseAction: invalid size",
    "parseAction: content has",
])
bucseTests.testCleanup()

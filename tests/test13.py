#!/bin/python3

# Data from the (untrusted) server that is larger than bucse-mount's buffers
# must be rejected, not decrypted past the end of a buffer: action files
# larger than MAX_ACTION_LEN (as files or inside a tar file), tar entries
# whose header claims more data than the tar file has, and storage blocks
# larger than the file's block size. Under valgrind an overflow shows up as
# an error.

import bucseTests
import errno
import hashlib
import json
import os
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

MAX_ACTION_LEN = 1024 * 1024


# Returns (name, encrypted data) of an action file whose JSON is padded with
# whitespace to paddedSize bytes, so it is valid apart from its size.
def makeAction(action, paddedSize = 0):
    data = json.dumps([action]).encode("utf-8")
    data += b" " * max(0, paddedSize - len(data))
    return hashlib.sha1(data).hexdigest(), bucseTests.encryptForRepo(data)


def addDirectoryAction(path, paddedSize = 0):
    return makeAction({
        "time": int(time.time() * 1000000),
        "action": "addDirectory",
        "path": path,
        "content": [],
        "size": 0,
        "blockSize": 0,
    }, paddedSize)


def mountPath(path):
    return "test_%d/%s" % (bucseTests.pid, path)


# The local destination looks for new action files only every few seconds,
# so everything is written first and handled in one go.

# 1. An action file larger than MAX_ACTION_LEN is rejected
name, data = addDirectoryAction("oversized", MAX_ACTION_LEN + 1000)
bucseTests.writeRepoFile("actions/%s" % name, data)

# 2. The same inside a tar file: only that entry is ignored
packed = [addDirectoryAction("packed_ok_1"),
          addDirectoryAction("packed_oversized", MAX_ACTION_LEN + 1000),
          addDirectoryAction("packed_ok_2")]
bucseTests.writeRepoFile("actions/oversized.tar", bucseTests.makeActionsTar(packed))

# 3. A tar entry whose header claims more data than the whole tar file has
tarData = bytearray(bucseTests.makeActionsTar([addDirectoryAction("bad_size")]))
tarData[124:136] = b"%011o\0" % (len(tarData) * 2)
tarData[148:156] = b" " * 8
tarData[148:156] = b"%06o\0 " % sum(tarData[0:512])
bucseTests.writeRepoFile("actions/badsize.tar", bytes(tarData))

# 4. Storage blocks larger than the file's block size: one that doesn't fit
# in the decryption buffer, one that fits only in its margin. A block of
# exactly the block size is fine.
BLOCK_SIZE = 512
blockFiles = {}
for fileName, blockLen in [("block_ok", BLOCK_SIZE),
                           ("block_margin", BLOCK_SIZE + 8),
                           ("block_large", 2 * BLOCK_SIZE - 64)]:
    blockData = os.urandom(blockLen)
    blockName = hashlib.sha1(blockData).hexdigest()
    bucseTests.writeRepoFile("storage/%s" % blockName, bucseTests.encryptForRepo(blockData))
    name, data = makeAction({
        "time": int(time.time() * 1000000),
        "action": "addFile",
        "path": fileName,
        "content": [blockName],
        "size": BLOCK_SIZE,
        "blockSize": BLOCK_SIZE,
    })
    bucseTests.writeRepoFile("actions/%s" % name, data)
    blockFiles[fileName] = blockData

bucseTests.waitForActionsHandled()

for path in ["oversized", "packed_oversized", "bad_size"]:
    assert not os.path.exists(mountPath(path)), "%s exists" % path
for path in ["packed_ok_1", "packed_ok_2"] + list(blockFiles):
    assert os.path.exists(mountPath(path)), "%s is missing" % path

for fileName, blockData in blockFiles.items():
    try:
        with open(mountPath(fileName), "rb") as f:
            content = f.read()
        assert fileName == "block_ok", "%s was read" % fileName
        assert content == blockData[:BLOCK_SIZE]
    except OSError as e:
        assert fileName != "block_ok", "block_ok can't be read: %s" % e
        assert e.errno == errno.EIO, "%s: %s" % (fileName, e)

bucseTests.verifyFailOnError(True, allowedErrors = [
    # the oversized action files
    "encrAesDecrypt: encrypted data too large for the buffer",
    "actionAddedDecryptOneAction: decrypt failed",
    # the tar entry with a wrong size
    "forEveryFileInTar: invalid entry size",
    "actionAddedDecrypt: tar file handling failed",
    # the oversized storage blocks
    "decryptBlock: decrypt failed",
    "decryptBlock: decrypted block too large",
])
bucseTests.testCleanup()

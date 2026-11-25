#!/bin/python3

# Action files that can't be handled yet when bucse-mount first sees them (not
# completely uploaded, not readable) must be read again later, not ignored
# until a remount.

import bucseTests
import io
import os
import tarfile


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

# 1. An action file whose upload gets interrupted ...
partialAction = "data/partialAction/actions/37eeeee92b3c889b0696208032c1a237e8be2b5a"
for name in os.listdir("data/partialAction/actions"):
    if name != os.path.basename(partialAction):
        bucseTests.copyAction("data/partialAction/actions/%s" % name)
bucseTests.copyActionPartially(partialAction)
bucseTests.waitForActionsHandled()
# ... and is completed later
bucseTests.copyAction(partialAction)
bucseTests.waitForFiles([str(i) for i in range(10)])

# 2. The same with a tar file of packed actions: cut exactly between two
# entries (which unpacks without an error, only the end-of-archive marker is
# missing), then inside the data of its last entry
packed = ["packed_%d" % i for i in range(3)]
tarData = bucseTests.makeActionsTar([bucseTests.makeAddDirectoryAction(p) for p in packed])
with tarfile.open(fileobj=io.BytesIO(tarData)) as tar:
    lastEntry = tar.getmembers()[-1]
for cut in [lastEntry.offset, lastEntry.offset_data + 10]:
    bucseTests.writeRepoFile("actions/packed.tar", tarData[:cut])
    bucseTests.waitForActionsHandled()
bucseTests.writeRepoFile("actions/packed.tar", tarData)
bucseTests.waitForFiles(packed)

# 3. An action file that can't be read at first (when the tests run as root,
# it is readable anyway)
unreadableName, unreadableData = bucseTests.makeAddDirectoryAction("unreadable")
bucseTests.writeRepoFile("actions/%s" % unreadableName, unreadableData)
bucseTests.chmodRepoFile("actions/%s" % unreadableName, 0)
bucseTests.waitForActionsHandled()
bucseTests.chmodRepoFile("actions/%s" % unreadableName, 0o644)
bucseTests.waitForFiles(["unreadable"])

bucseTests.verifyFailOnError(True, allowedErrors = [
    # the partial files are rejected
    "parseAction: json_tokener_parse_ex()",
    "encrAesDecrypt: EVP_DecryptFinal failed",
    "actionAddedDecryptOneAction: decrypt failed",
    "forEveryFileInTar: no end-of-archive marker",
    "actionAddedDecrypt: tar file handling failed",
    # the unreadable file
    "fopen(): Permission denied",
    "sftp_open(): SFTP server: Permission denied",
])
bucseTests.testCleanup()

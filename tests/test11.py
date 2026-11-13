#!/bin/python3

import bucseTests
import time
import os


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

partialAction = "data/partialAction/actions/37eeeee92b3c889b0696208032c1a237e8be2b5a"
for name in os.listdir("data/partialAction/actions"):
    if name != os.path.basename(partialAction):
        bucseTests.copyAction("data/partialAction/actions/%s" % name)
# the upload of one action file gets interrupted ...
bucseTests.copyActionPartially(partialAction)
time.sleep(25)
# ... and completed later
bucseTests.copyAction(partialAction)
time.sleep(40)

bucseTests.verifyFailOnError(True)
bucseTests.testCleanup()

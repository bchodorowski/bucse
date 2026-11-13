#!/bin/python3

import bucseTests


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

bucseTests.copyActions("data/duplicates/actions_0_begin")
bucseTests.waitForFiles([str(i) for i in range(10)])
# duplicates of already handled actions
bucseTests.copyActions("data/duplicates/actions_0_end")
bucseTests.waitForActionsHandled()

#bucseTests.verifyWithMirror()
bucseTests.verifyFailOnError()
bucseTests.testCleanup()

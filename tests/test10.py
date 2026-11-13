#!/bin/python3

import bucseTests


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

bucseTests.copyActions("data/duplicates/actions_0_begin")
bucseTests.waitForFiles([str(i) for i in range(10)])
# duplicates of already handled actions, packed into a tar file
bucseTests.copyActionsAsTar("data/duplicates/actions_0_end", "actions_0_end.tar")
bucseTests.waitForActionsHandled()
# ... and the packed action files removed, as a packing tool does
bucseTests.deleteAction("37eeeee92b3c889b0696208032c1a237e8be2b5a")
bucseTests.deleteAction("cd526d9b8d69c5de4eeb9e8dba1fc9be74b83be8")
bucseTests.deleteAction("f2caf745bee043c892dc864c4722102df78aa089")
bucseTests.waitForActionsHandled()

bucseTests.verifyFailOnError()
bucseTests.testCleanup()

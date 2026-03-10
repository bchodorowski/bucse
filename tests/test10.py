#!/bin/python3

import bucseTests
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

bucseTests.copyActions("data/duplicates/actions_0_begin")
time.sleep(25)
bucseTests.copyAction("data/duplicates/actions_0_end.tar")
time.sleep(40)
bucseTests.deleteAction("37eeeee92b3c889b0696208032c1a237e8be2b5a")
bucseTests.deleteAction("cd526d9b8d69c5de4eeb9e8dba1fc9be74b83be8")
bucseTests.deleteAction("f2caf745bee043c892dc864c4722102df78aa089")
time.sleep(40)

bucseTests.verifyFailOnError()
bucseTests.testCleanup()

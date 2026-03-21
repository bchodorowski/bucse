#!/bin/python3

import bucseTests
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

bucseTests.copyActions("data/partialAction/actions_0_begin")
time.sleep(25)
bucseTests.copyActions("data/partialAction/actions_0_end")
time.sleep(40)

bucseTests.verifyFailOnError(True)
bucseTests.testCleanup()

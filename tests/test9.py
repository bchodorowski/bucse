#!/bin/python3

import bucseTests
import time


bucseTests.parseArgs()


bucseTests.failOnError = True


bucseTests.mountDirs()

bucseTests.copyActions("data/duplicates/actions_0_begin")
time.sleep(25)

#bucseTests.verifyWithMirror()
bucseTests.verifyFailOnError()
bucseTests.testCleanup()

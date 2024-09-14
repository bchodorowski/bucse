#!/bin/python3

import bucseTests


bucseTests.downloadTmp(
        'https://mirrors.edge.kernel.org/pub/linux/kernel/v6.x/linux-6.9.4.tar.xz',
        'linux-6.9.4.tar.xz',
        '8e4ef26770b4ce8db7d988fe9c9e721762f30a36')
bucseTests.mountDirs()
bucseTests.mirrorCommand(["cp", "tmp/linux-6.9.4.tar.xz" ,"__TESTDIR__/"])
bucseTests.verifyWithMirror()
bucseTests.testCleanup()

#!/bin/sh
#
# Runs the stress tests (stressTest*.py) for every backend combination, without
# valgrind by default. They copy multi-gigabyte files: they need a lot of time,
# RAM and about 16 GB of free disk space. Options and environment variables
# are the same as for allTests.sh.

exec "$(dirname "$0")/allTests.sh" --stress --no-valgrind "$@"

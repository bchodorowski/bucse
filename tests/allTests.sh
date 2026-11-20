#!/bin/sh
#
# Runs the tests for every combination of encryption backend (none, aes) and
# destination (local, and ssh when BUCSE_TEST_SSH is set).
#
# Usage: ./allTests.sh [--no-valgrind] [--valgrind] [--stress] [test...]
#   --no-valgrind  run bucse-mount without valgrind (faster, no memory checks)
#   --valgrind     run bucse-mount under valgrind (the default)
#   --stress       run the stress tests (stressTest*.py) instead of test*.py
#   test...        run only the given tests, e.g. test3.py test12.py
#
# Environment:
#   BUCSE_TEST_SSH         also test the ssh destination, with repositories
#                          created under this address, e.g.
#                          ssh://localhost/~/bucseTests
#   BUCSE_TEST_PASSPHRASE  passphrase for aes repositories (default: 12345)
#
# The output of every run is kept in logs/<test>_<encryption>_<destination>.log.
# A failed run is cleaned up (unmounted, its directories and repository
# removed). The test's pid, its valgrind log (tmp/valgrind_<pid>.txt),
# bucse-mount's output (tmp/mount_<pid>.txt) and a list of the running
# processes (something else, e.g. a file indexer, may have used the mount) are
# appended to its log; the files in tmp/ are kept too.
# The exit code is non-zero if any run failed.
#
# When interrupted (Ctrl+C, SIGTERM, SIGHUP), the running test is stopped and
# cleaned up the same way as a failed one, and the script exits.

cd "$(dirname "$0")" || exit 1

VALGRIND="--valgrind"
PATTERN="test[0-9]*.py"
TESTS=""
PASSPHRASE="${BUCSE_TEST_PASSPHRASE:-12345}"

for arg in "$@"; do
	case "$arg" in
		--no-valgrind) VALGRIND="" ;;
		--valgrind) VALGRIND="--valgrind" ;;
		--stress) PATTERN="stressTest[0-9]*.py" ;;
		-*) echo "unknown option: $arg" >&2; exit 2 ;;
		*) TESTS="$TESTS $arg" ;;
	esac
done

if [ -z "$TESTS" ]; then
	TESTS=$(ls $PATTERN | sort -V)
fi

DESTINATIONS="local"
if [ -n "$BUCSE_TEST_SSH" ]; then
	DESTINATIONS="local ssh"
fi

mkdir -p logs
RESULTS=$(mktemp)
FAILED=0

# cleanUp <pid> <repository path>: removes what a failed test left behind
cleanUp() {
	if mountpoint -q "test_$1" 2>/dev/null; then
		fusermount3 -u "test_$1" 2>/dev/null || fusermount3 -uz "test_$1"
	fi
	# a bucse-mount that is still running (e.g. not mounted yet); wait until
	# it exits (up to 30 s, valgrind needs a while)
	pkill -f "test_$1_repo" 2>/dev/null
	i=0
	while pgrep -f "test_$1_repo" > /dev/null && [ $i -lt 60 ]; do
		sleep 0.5
		i=$((i + 1))
	done
	rm -rf "test_$1" "test_$1_mirror"
	python3 -c 'import sys, bucseTests
bucseTests.argRepoPath = sys.argv[1]
bucseTests.pid = int(sys.argv[2])
bucseTests.removeRepo()' "$2" "$1"
}

# appendProcesses <log>: appends the running processes, right after a test
# failed (something else, e.g. a file indexer, may have used the mount)
appendProcesses() {
	{
		echo
		echo "=== running processes:"
		ps aux
	} >> "$1" 2>&1
}

# appendLogs <pid> <log>: appends the failed test's pid and logs; called after
# cleanUp, so that a bucse-mount under valgrind has exited and its log is
# complete
appendLogs() {
	{
		echo
		echo "=== test pid: $1"
		for f in "tmp/valgrind_$1.txt" "tmp/mount_$1.txt"; do
			if [ -f "$f" ]; then
				echo "=== $f:"
				cat "$f"
			fi
		done
	} >> "$2" 2>&1
}

# The running test (pid, repoPath, log) is cleaned up when the script is
# interrupted. A test runs in the background, so it ignores SIGINT: it is
# stopped with SIGTERM.
pid=""
interrupted() {
	trap '' INT TERM HUP
	echo
	echo "Interrupted, cleaning up..."
	if [ -n "$pid" ]; then
		kill -TERM "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
		appendProcesses "$log"
		cleanUp "$pid" "$repoPath" >> "$log" 2>&1
		appendLogs "$pid" "$log"
		echo "The interrupted test's log: $log"
	fi
	rm -f "$RESULTS"
	exit "$1"
}
trap 'interrupted 130' INT
trap 'interrupted 143' TERM
trap 'interrupted 129' HUP

for destination in $DESTINATIONS; do
	if [ "$destination" = "ssh" ]; then
		repoPath="$BUCSE_TEST_SSH"
	else
		repoPath="."
	fi
	for encryption in none aes; do
		for test in $TESTS; do
			name="${test%.py}_${encryption}_${destination}"
			log="logs/$name.log"
			printf '%-30s ' "$name"
			start=$(date +%s)

			./"$test" -r "$repoPath" -e "$encryption" -p "$PASSPHRASE" $VALGRIND > "$log" 2>&1 &
			pid=$!
			wait $pid
			result=$?
			testPid=$pid
			pid=""

			duration=$(( $(date +%s) - start ))
			if [ $result -eq 0 ]; then
				status="ok"
			else
				status="FAILED"
				FAILED=1
				appendProcesses "$log"
				cleanUp $testPid "$repoPath" >> "$log" 2>&1
				appendLogs $testPid "$log"
			fi
			echo "$status (${duration}s)"
			echo "$name $status" >> "$RESULTS"
		done
	done
done

echo
if [ $FAILED -eq 0 ]; then
	echo "All tests passed."
else
	echo "Failed:"
	grep ' FAILED$' "$RESULTS" | sed 's/ FAILED$//; s/^/  /'
	echo "See the logs in logs/."
fi
rm -f "$RESULTS"
exit $FAILED

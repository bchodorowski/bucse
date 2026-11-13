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
# removed); its log and valgrind log (tmp/valgrind_<pid>.txt) are kept.
# The exit code is non-zero if any run failed.

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
	rm -rf "test_$1" "test_$1_mirror"
	python3 -c 'import sys, bucseTests
bucseTests.argRepoPath = sys.argv[1]
bucseTests.pid = int(sys.argv[2])
bucseTests.removeRepo()' "$2" "$1"
}

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

			duration=$(( $(date +%s) - start ))
			if [ $result -eq 0 ]; then
				status="ok"
			else
				status="FAILED"
				FAILED=1
				cleanUp $pid "$repoPath" >> "$log" 2>&1
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

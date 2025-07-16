#!/bin/sh

REPO_PATH="."
#REPO_PATH="ssh://example.com/~/bucseTests"

#ENCRYPTION="none"
ENCRYPTION="aes"

PASSWORD="12345"

VALGRIND="--valgrind"
#VALGRIND=""

#DEBUG="--debug"
DEBUG=""

echo "========== test 1 =========="
./test1.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
echo "========== test 2 =========="
./test2.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
echo "========== test 3 =========="
./test3.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
echo "========== test 4 =========="
./test4.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
echo "========== test 5 =========="
./test5.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
echo "========== test 6 =========="
./test6.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
echo "========== test 7 =========="
./test7.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
echo "========== test 8 =========="
./test8.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG

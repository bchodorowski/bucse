#!/bin/sh

REPO_PATH="."
#REPO_PATH="ssh://example.com/~/bucseTests"

ENCRYPTION="none"
#ENCRYPTION="aes"

PASSWORD="12345"

VALGRIND="--valgrind"
#VALGRIND=""

#DEBUG="--debug"
DEBUG=""

./test1.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
./test2.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
./test3.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
./test4.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG
./test5.py -r $REPO_PATH -e $ENCRYPTION -p $PASSWORD $VALGRIND $DEBUG

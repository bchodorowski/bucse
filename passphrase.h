#ifndef PASSPHRASE_H
#define PASSPHRASE_H

// Reading the passphrase: from a file or on the terminal, never from the
// command line (which every local user can see). The functions return a
// malloc()ed string, to be wiped with wipeMemory() and freed, or NULL if
// there is no passphrase (an error is logged, except for an empty one typed
// on the terminal).

// The first line of a file, without its newline. "-" reads stdin.
char* readPassphraseFile(const char* path);

// Non-zero if stdin and stdout are a terminal, so the passphrase can be
// asked for with readPassphraseTerminal().
int passphraseTerminalAvailable();

// Asks for the passphrase on the terminal, with echo off. confirm: ask for
// it twice, and fail if the two don't match.
char* readPassphraseTerminal(int confirm);

#endif

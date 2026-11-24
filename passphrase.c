#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "conf.h"
#include "log.h"
#include "passphrase.h"

#define MAX_PASSPHRASE_LEN 1023

// Reads one line from fd, a byte at a time with read(): no stdio buffer, so
// no copy of the passphrase is left anywhere but in buf, which is wiped.
// Returns the line without its newline, NULL on a read error or for a line
// longer than MAX_PASSPHRASE_LEN (both logged), or for an empty one (not
// logged: the caller knows what it means).
static char* readPassphraseLine(int fd, const char* name)
{
	char buf[MAX_PASSPHRASE_LEN + 1];
	size_t len = 0;
	int complete = 0;
	char* result = NULL;

	while (len < sizeof(buf)) {
		ssize_t r = read(fd, buf + len, 1);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r < 0) {
			logPrintf(LOG_ERROR, "reading the passphrase from %s: %s\n",
				name, strerror(errno));
			goto out;
		}
		if (r == 0 || buf[len] == '\n') {
			complete = 1;
			break;
		}
		len++;
	}
	if (!complete) {
		logPrintf(LOG_ERROR, "the passphrase in %s is longer than %d bytes\n",
			name, MAX_PASSPHRASE_LEN);
		goto out;
	}
	if (len == 0) {
		goto out;
	}

	result = malloc(len + 1);
	if (result == NULL) {
		logPrintf(LOG_ERROR, "readPassphraseLine: malloc(): %s\n", strerror(errno));
		goto out;
	}
	memcpy(result, buf, len);
	result[len] = 0;

out:
	wipeMemory(buf, sizeof(buf));
	return result;
}

char* readPassphraseFile(const char* path)
{
	if (strcmp(path, "-") == 0) {
		char* result = readPassphraseLine(STDIN_FILENO, "stdin");
		if (result == NULL) {
			logPrintf(LOG_ERROR, "no passphrase on stdin\n");
		}
		return result;
	}

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		logPrintf(LOG_ERROR, "can't open the passphrase file %s: %s\n",
			path, strerror(errno));
		return NULL;
	}
	char* result = readPassphraseLine(fd, path);
	close(fd);
	if (result == NULL) {
		logPrintf(LOG_ERROR, "no passphrase in %s\n", path);
	}
	return result;
}

int passphraseTerminalAvailable()
{
	return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

// Prints the prompt and reads a line with echo off. The original terminal
// settings are restored right after reading, also when reading fails.
static char* promptOnce(const char* prompt)
{
	fprintf(stdout, "%s", prompt);
	fflush(stdout);

	struct termios termOrig;
	int termSaved = (tcgetattr(STDIN_FILENO, &termOrig) == 0);
	if (termSaved) {
		struct termios term = termOrig;
		term.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSANOW, &term);
	}

	char* result = readPassphraseLine(STDIN_FILENO, "the terminal");

	if (termSaved) {
		tcsetattr(STDIN_FILENO, TCSANOW, &termOrig);
	}
	fprintf(stdout, "\n");
	fflush(stdout);
	return result;
}

char* readPassphraseTerminal(int confirm)
{
	char* result = promptOnce("Passphrase: ");
	if (result == NULL || !confirm) {
		return result;
	}

	char* repeated = promptOnce("Repeat the passphrase: ");
	if (repeated == NULL || strcmp(result, repeated) != 0) {
		logPrintf(LOG_ERROR, "the passphrases don't match\n");
		wipeMemory(result, strlen(result));
		free(result);
		result = NULL;
	}
	if (repeated != NULL) {
		wipeMemory(repeated, strlen(repeated));
		free(repeated);
	}
	return result;
}

/*
 * bucse-init.c
 *
 * The program for creating and initializing a bucse repository.
 */

#include <stdio.h>
#include <unistd.h>
#include <ctype.h>

#include <json.h>

#include "destinations/dest.h"

int main(int argc, char *argv[])
{
	char *passphrase = NULL;

	opterr = 0;

	int c;
	while ((c = getopt (argc, argv, "Vhp:")) != -1) {
		switch (c) {
			case 'V':
				// TODO: version
				break;
			case 'h':
				// TODO: help
				break;
			case 'p':
				passphrase = optarg;
				break;
			case '?':
				if (optopt == 'p')
					fprintf (stderr, "Option -%c requires an argument.\n", optopt);
				else if (isprint (optopt))
					fprintf (stderr, "Unknown option `-%c'.\n", optopt);
				else
					fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
				return 1;
			default:
				abort ();
		}
	}


	printf ("passphrase = %s\n", passphrase);

	int index;
	for (index = optind; index < argc; index++)
		printf ("Non-option argument %s\n", argv[index]);

	// TODO

	return 0;
}


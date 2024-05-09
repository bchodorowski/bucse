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

Destination *destination;

int initRepo(char* repository, char* passphrase, char* encryption)
{
	char* realPath;
	getDestinationBasedOnPathPrefix(&destination, &realPath, repository);
	int err = destination->init(realPath);
	if (err != 0)
	{
		fprintf(stderr, "destination->init(): %d\n", err);
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	char *passphrase = NULL;
	char *encryption = NULL;

	opterr = 0;

	int c;
	while ((c = getopt (argc, argv, "Vhp:e:")) != -1) {
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
			case 'e':
				encryption = optarg;
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
	printf ("encryption = %s\n", encryption);

	int index;
	int ret = 0;
	for (index = optind; index < argc; index++)
		ret += initRepo(argv[index], passphrase, encryption);

	return ret;
}


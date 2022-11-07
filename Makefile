CC=gcc
CFLAGS=`pkg-config fuse --cflags` `pkg-config json-c --cflags`
LIBS=`pkg-config fuse --libs` `pkg-config json-c --libs`

bucse: bucse.o destinations/dest_local.o encryption/encr_none.o
	$(CC) -o bucse $(CFLAGS) bucse.o destinations/dest_local.o encryption/encr_none.o $(LIBS)

bucse.o: bucse.c destinations/dest.h encryption/encr.h
	$(CC) -c bucse.c $(CFLAGS)

destinations/dest_local.o: destinations/dest_local.c destinations/dest.h
	$(CC) -c destinations/dest_local.c -o destinations/dest_local.o $(CFLAGS)

encryption/encr_none.o: encryption/encr_none.c encryption/encr.h
	$(CC) -c encryption/encr_none.c -o encryption/encr_none.o $(CFLAGS)

clean:
	-rm -f bucse bucse.o destinations/dest_local.o encryption/encr_none.o

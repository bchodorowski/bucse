CC=gcc -g
CFLAGS=`pkg-config fuse3 --cflags` `pkg-config json-c --cflags`
LIBS=`pkg-config fuse3 --libs` `pkg-config json-c --libs` -lpthread -lssl -lcrypto

bucse: bucse.o destinations/dest_local.o encryption/encr_none.o encryption/encr_aes.o dynarray.o filesystem.o actions.o
	$(CC) -o bucse $(CFLAGS) bucse.o destinations/dest_local.o encryption/encr_none.o encryption/encr_aes.o dynarray.o filesystem.o actions.o $(LIBS)

bucse.o: bucse.c destinations/dest.h encryption/encr.h dynarray.h filesystem.h actions.h
	$(CC) -c bucse.c $(CFLAGS)

destinations/dest_local.o: destinations/dest_local.c destinations/dest.h
	$(CC) -c destinations/dest_local.c -o destinations/dest_local.o $(CFLAGS)

encryption/encr_none.o: encryption/encr_none.c encryption/encr.h
	$(CC) -c encryption/encr_none.c -o encryption/encr_none.o $(CFLAGS)

encryption/encr_aes.o: encryption/encr_aes.c encryption/encr.h
	$(CC) -c encryption/encr_aes.c -o encryption/encr_aes.o $(CFLAGS)

dynarray.o: dynarray.h
	$(CC) -c dynarray.c -o dynarray.o $(CFLAGS)

filesystem.o: filesystem.h dynarray.h
	$(CC) -c filesystem.c -o filesystem.o $(CFLAGS)

actions.o: actions.h dynarray.h filesystem.h
	$(CC) -c actions.c -o actions.o $(CFLAGS)

clean:
	-rm -f bucse bucse.o destinations/dest_local.o encryption/encr_none.o encryption/encr_aes.o dynarray.o filesystem.o actions.o

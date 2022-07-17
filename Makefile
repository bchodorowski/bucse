CC=gcc
CFLAGS=`pkg-config fuse --cflags`
LIBS=`pkg-config fuse --libs`

bucse: bucse.o
	$(CC) -o bucse $(CFLAGS) bucse.o $(LIBS)

bucse.o: bucse.c
	$(CC) -c bucse.c $(CFLAGS)

clean:
	-rm -f bucse bucse.o

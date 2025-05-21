CC=gcc -g -DFUSE_USE_VERSION=34
CFLAGS=`pkg-config fuse3 --cflags` `pkg-config json-c --cflags` -Wall -pedantic
LIBS=`pkg-config fuse3 --libs` `pkg-config json-c --libs` -lpthread -lssl -lcrypto -lssh

all: bucse-mount bucse-init

bucse-mount: bucse-mount.o \
	destinations/dest.o \
	destinations/dest_local.o \
	destinations/dest_ssh.o \
	encryption/encr.o \
	encryption/encr_none.o \
	encryption/encr_aes.o \
	dynarray.o \
	filesystem.o \
	actions.o \
	time.o \
	conf.o \
	log.o \
	cache.o \
	operations/operations.o \
	operations/getattr.o \
	operations/flush.o \
	operations/readdir.o \
	operations/open.o \
	operations/create.o \
	operations/release.o \
	operations/read.o \
	operations/write.o \
	operations/unlink.o \
	operations/mkdir.o \
	operations/rmdir.o \
	operations/truncate.o \
	operations/rename.o \
	operations/init.o
	$(CC) -o bucse-mount $(CFLAGS) bucse-mount.o \
		destinations/dest.o \
		destinations/dest_local.o \
		destinations/dest_ssh.o \
		encryption/encr.o \
		encryption/encr_none.o \
		encryption/encr_aes.o \
		dynarray.o \
		filesystem.o \
		actions.o \
		time.o \
		conf.o \
		log.o \
		cache.o \
		operations/operations.o \
		operations/getattr.o \
		operations/flush.o \
		operations/readdir.o \
		operations/open.o \
		operations/create.o \
		operations/release.o \
		operations/read.o \
		operations/write.o \
		operations/unlink.o \
		operations/mkdir.o \
		operations/rmdir.o \
		operations/truncate.o \
		operations/rename.o \
		operations/init.o \
		$(LIBS)

bucse-mount.o: bucse-mount.c \
	destinations/dest.h \
	encryption/encr.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	conf.h \
	log.h \
	cache.h \
	operations/operations.h \
	operations/getattr.h \
	operations/flush.h \
	operations/readdir.h \
	operations/open.h \
	operations/create.h \
	operations/release.h \
	operations/read.h \
	operations/write.h \
	operations/unlink.h \
	operations/mkdir.h \
	operations/rmdir.h \
	operations/truncate.h \
	operations/rename.h \
	operations/init.h
	$(CC) -c bucse-mount.c $(CFLAGS)

destinations/dest.o: destinations/dest.c \
	log.h \
	destinations/dest.h
	$(CC) -c destinations/dest.c -o destinations/dest.o $(CFLAGS)

destinations/dest_local.o: destinations/dest_local.c \
	log.h \
	destinations/dest.h
	$(CC) -c destinations/dest_local.c -o destinations/dest_local.o $(CFLAGS)

destinations/dest_ssh.o: destinations/dest_ssh.c \
	destinations/dest.h
	$(CC) -c destinations/dest_ssh.c -o destinations/dest_ssh.o $(CFLAGS)

encryption/encr.o: encryption/encr.c \
	log.h \
	encryption/encr.h
	$(CC) -c encryption/encr.c -o encryption/encr.o $(CFLAGS)

encryption/encr_none.o: encryption/encr_none.c \
	encryption/encr.h
	$(CC) -c encryption/encr_none.c -o encryption/encr_none.o $(CFLAGS)

encryption/encr_aes.o: encryption/encr_aes.c \
	log.h \
	encryption/encr.h
	$(CC) -c encryption/encr_aes.c -o encryption/encr_aes.o $(CFLAGS)

dynarray.o: dynarray.c \
	log.h \
	dynarray.h
	$(CC) -c dynarray.c -o dynarray.o $(CFLAGS)

filesystem.o: filesystem.c \
	log.h \
	dynarray.h \
	filesystem.h
	$(CC) -c filesystem.c -o filesystem.o $(CFLAGS)

actions.o: actions.c \
	actions.h \
	dynarray.h \
	filesystem.h \
	log.h
	$(CC) -c actions.c -o actions.o $(CFLAGS)

time.o: time.c
	$(CC) -c time.c -o time.o $(CFLAGS)

conf.o: conf.c \
	conf.h
	$(CC) -c conf.c -o conf.o $(CFLAGS)

log.o: log.c \
	conf.h \
	log.h
	$(CC) -c log.c -o log.o $(CFLAGS)

cache.o: cache.c \
	log.h \
	dynarray.h \
	cache.h
	$(CC) -c cache.c -o cache.o $(CFLAGS)

operations/operations.o: operations/operations.c \
	operations/operations.h \
	actions.h \
	destinations/dest.h \
	log.h \
	conf.h \
	cache.h \
	encryption/encr.h
	$(CC) -c operations/operations.c -o operations/operations.o $(CFLAGS)

operations/getattr.o: operations/getattr.c \
	operations/getattr.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	log.h \
	operations/operations.h \
	operations/flush.h
	$(CC) -c operations/getattr.c -o operations/getattr.o $(CFLAGS)

operations/flush.o: operations/flush.c \
	operations/flush.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	conf.h \
	destinations/dest.h \
	encryption/encr.h \
	operations/operations.h
	$(CC) -c operations/flush.c -o operations/flush.o $(CFLAGS)

operations/readdir.o: operations/readdir.c \
	operations/readdir.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	operations/operations.h
	$(CC) -c operations/readdir.c -o operations/readdir.o $(CFLAGS)

operations/open.o: operations/open.c \
	operations/open.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	operations/operations.h
	$(CC) -c operations/open.c -o operations/open.o $(CFLAGS)

operations/create.o: operations/create.c \
	operations/create.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	operations/operations.h \
	operations/open.h
	$(CC) -c operations/create.c -o operations/create.o $(CFLAGS)

operations/release.o: operations/release.c \
	operations/release.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	log.h \
	operations/operations.h \
	operations/flush.h
	$(CC) -c operations/release.c -o operations/release.o $(CFLAGS)

operations/read.o: operations/read.c \
	operations/read.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	conf.h \
	destinations/dest.h \
	encryption/encr.h \
	operations/operations.h \
	operations/flush.h
	$(CC) -c operations/read.c -o operations/read.o $(CFLAGS)

operations/write.o: operations/write.c \
	operations/write.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	log.h \
	operations/operations.h \
	operations/flush.h
	$(CC) -c operations/write.c -o operations/write.o $(CFLAGS)

operations/unlink.o: operations/unlink.c \
	operations/unlink.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	operations/operations.h
	$(CC) -c operations/unlink.c -o operations/unlink.o $(CFLAGS)

operations/mkdir.o: operations/mkdir.c \
	operations/mkdir.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	operations/operations.h
	$(CC) -c operations/mkdir.c -o operations/mkdir.o $(CFLAGS)

operations/rmdir.o: operations/rmdir.c \
	operations/rmdir.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	operations/operations.h
	$(CC) -c operations/rmdir.c -o operations/rmdir.o $(CFLAGS)

operations/truncate.o: operations/truncate.c \
	operations/truncate.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	log.h \
	operations/operations.h \
	operations/flush.h
	$(CC) -c operations/truncate.c -o operations/truncate.o $(CFLAGS)

operations/rename.o: operations/rename.c \
	operations/rename.h \
	dynarray.h \
	filesystem.h \
	actions.h \
	time.h \
	log.h \
	operations/operations.h
	$(CC) -c operations/rename.c -o operations/rename.o $(CFLAGS)

operations/init.o: operations/init.c \
	operations/init.h \
	actions.h \
	operations/operations.h
	$(CC) -c operations/init.c -o operations/init.o $(CFLAGS)

bucse-init: bucse-init.o \
	conf.o \
	log.o \
	time.o \
	destinations/dest.o \
	destinations/dest_local.o \
	destinations/dest_ssh.o \
	encryption/encr.o \
	encryption/encr_none.o \
	encryption/encr_aes.o
	$(CC) -o bucse-init $(CFLAGS) bucse-init.o \
		conf.o \
		log.o \
		time.o \
		destinations/dest.o \
		destinations/dest_local.o \
		destinations/dest_ssh.o \
		encryption/encr.o \
		encryption/encr_none.o \
		encryption/encr_aes.o \
		$(LIBS)

bucse-init.o: bucse-init.c \
	time.o \
	conf.o \
	log.o \
	destinations/dest.h \
	encryption/encr.h
	$(CC) -c bucse-init.c $(CFLAGS)

clean:
	-rm -f bucse-mount bucse-mount.o \
		destinations/dest.o \
		destinations/dest_local.o \
		destinations/dest_ssh.o \
		encryption/encr.o \
		encryption/encr_none.o \
		encryption/encr_aes.o \
		dynarray.o \
		filesystem.o \
		actions.o \
		time.o \
		conf.o \
		log.o \
		cache.o \
		operations/operations.o \
		operations/getattr.o \
		operations/flush.o \
		operations/readdir.o \
		operations/open.o \
		operations/create.o \
		operations/release.o \
		operations/read.o \
		operations/write.o \
		operations/unlink.o \
		operations/mkdir.o \
		operations/rmdir.o \
		operations/truncate.o \
		operations/rename.o \
		operations/init.o \
		bucse-init bucse-init.o

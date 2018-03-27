# DEBUG changes behaviour completly and is for developing / debugging the application
# DEBUG disabled is the correct setting

CC=gcc
CFLAGS=-O2 -Wall
LDFLAGS=
# DEBUG=-DDEBUG -g
PREFIX=/usr/local

ts2shout: ts2shout.o mpa_header.o util.o pes.o crc32.o
	${CC} ${DEBUG} ${LDFLAGS} -o ts2shout ts2shout.o mpa_header.o util.o pes.o crc32.o
ts2shout.o: ts2shout.c ts2shout.h config.h
	${CC} ${DEBUG} ${CFLAGS} -c ts2shout.c
mpa_header.o: mpa_header.c config.h mpa_header.h
	${CC} ${DEBUG} ${CFLAGS} -c mpa_header.c
pes.o: pes.c config.h
	${CC} ${DEBUG} ${CFLAGS} -c pes.c
util.o: util.c
	${CC} ${DEBUG} ${CFLAGS} -c util.c
crc32.o: crc32.c
	${CC} ${DEBUG} ${CFLAGS} -c crc32.c

clean:
	rm -f *.o ts2shout

install: ts2shout
	install -g root -m 555 -o root ts2shout ${PREFIX}/bin/ts2shout
	install -D -g root -m 444 -o root ts2shout.1 ${PREFIX}/man/man1/ts2shout.1 


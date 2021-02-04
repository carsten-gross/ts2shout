# DEBUG changes behaviour completly and is for developing / debugging the application
# DEBUG disabled is the correct setting

CC ?= gcc
CFLAGS ?=-O2 -Wall
# DEBUG=-DDEBUG -g
PREFIX ?= /usr/local
CURRENT_VERSION:=$(shell git describe 2>/dev/null)
ifeq ($(CURRENT_VERSION),)
CURRENT_VERSION := "unknown"
endif
CURRENT_DATE:=$(shell date '+%d.%m.%Y %H:%M' 2>/dev/null)
ifeq ($(CURRENT_DATE),)
CURRENT_DATE := "unkown"
endif



ts2shout: ts2shout.o mpa_header.o util.o pes.o crc32.o rds.o
	${CC} ${DEBUG} ${LDFLAGS} -o ts2shout ts2shout.o rds.o mpa_header.o util.o pes.o crc32.o -lcurl
ts2shout.o: ts2shout.c ts2shout.h
	${CC} ${DEBUG} -DCURRENT_VERSION="${CURRENT_VERSION}" -DCURRENT_DATE="${CURRENT_DATE}" ${CFLAGS} -c ts2shout.c
mpa_header.o: mpa_header.c mpa_header.h
	${CC} ${DEBUG} ${CFLAGS} -c mpa_header.c
pes.o: pes.c 
	${CC} ${DEBUG} ${CFLAGS} -c pes.c
util.o: util.c
	${CC} ${DEBUG} ${CFLAGS} -c util.c
crc32.o: crc32.c
	${CC} ${DEBUG} ${CFLAGS} -c crc32.c
rds.o: rds.c
	${CC} ${DEBUG} ${CFLAGS} -c rds.c

clean:
	rm -f *.o ts2shout

install: ts2shout
	install -g root -m 555 -o root ts2shout ${PREFIX}/bin/ts2shout
	install -D -g root -m 444 -o root ts2shout.1 ${PREFIX}/man/man1/ts2shout.1 


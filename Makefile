# DEBUG changes behaviour completly and is for developing / debugging the application
# DEBUG disabled is the correct setting

CC ?= gcc
CFLAGS ?=-O2 -Wall
# DEBUG=-DDEBUG -g
PREFIX ?= /usr/local
SRCS=ts2shout.c pes.c mpa_header.c util.c crc32.c rds.c dsmcc.c

CURRENT_VERSION:=$(shell git describe 2>/dev/null)
ifeq ($(CURRENT_VERSION),)
CURRENT_VERSION := "unknown"
endif
CURRENT_DATE:=$(shell date '+%d.%m.%Y %H:%M' 2>/dev/null)
ifeq ($(CURRENT_DATE),)
CURRENT_DATE := "unkown"
endif

DEPDIR := .deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d

COMPILE.c = $(CC) -DCURRENT_VERSION="${CURRENT_VERSION}" -DCURRENT_DATE="${CURRENT_DATE}" $(DEPFLAGS) $(DEBUG) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c

%.o : %.c
%.o : %.c $(DEPDIR)/%.d | $(DEPDIR)
	$(COMPILE.c) $(OUTPUT_OPTION) $<

$(DEPDIR): ; @mkdir -p $@

DEPFILES := $(SRCS:%.c=$(DEPDIR)/%.d)

ts2shout: ts2shout.o mpa_header.o util.o pes.o crc32.o rds.o dsmcc.o
	${CC} ${DEBUG} ${LDFLAGS} -o ts2shout ts2shout.o rds.o mpa_header.o util.o pes.o crc32.o dsmcc.o -lcurl

clean:
	rm -f *.o ts2shout

install: ts2shout
	install -g root -m 555 -o root ts2shout ${PREFIX}/bin/ts2shout
	install -D -g root -m 444 -o root ts2shout.1 ${PREFIX}/man/man1/ts2shout.1 


$(DEPFILES):
include $(wildcard $(DEPFILES))


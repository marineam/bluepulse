CC = gcc
PKGLIB = libpulse libpulse-mainloop-glib glib-2.0 gio-unix-2.0
CFLAGS = -g -O2 -Wall -std=gnu99 -I. -D_GNU_SOURCE
CFLAGS += $(shell pkg-config $(PKGLIB) --cflags)
LIBS = $(shell pkg-config $(PKGLIB) --libs)

HEADERS = config.h $(wildcard src/*.h)
SRC_FILES = $(wildcard src/*.c) $(wildcard ccan/*/*.c)
OJB_FILES = $(SRC_FILES:.c=.o)

all: bluepulse

ccan/configurator: ccan/configurator.c
	$(CC) $(CFLAGS) -o $@ $<

config.h: ccan/configurator
	$< $(CC) $(CFLAGS) > $@ 

%.o: %.c Makefile $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

bluepulse: $(OJB_FILES)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

install: bluepulse
	install bluepulse /usr/local/bin

clean:
	$(RM) $(OJB_FILES) bluepulse config.h ccan/configurator

.PHONY: all install clean

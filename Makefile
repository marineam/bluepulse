CC = gcc
CFLAGS = -g -O2 -Wall -std=gnu99 -I. -D_GNU_SOURCE
CFLAGS += $(shell pkg-config libpulse --cflags)
LIBS = $(shell pkg-config libpulse --libs)

SRC_FILES = $(wildcard src/*.c) $(wildcard ccan/*/*.c)
OJB_FILES = $(SRC_FILES:.c=.o)

all: bluepulse

ccan/configurator: ccan/configurator.c
	$(CC) $(CFLAGS) -o $@ $<

config.h: ccan/configurator
	$< $(CC) $(CFLAGS) > $@ 

%.o: %.c Makefile config.h
	$(CC) $(CFLAGS) -c -o $@ $<

bluepulse: $(OJB_FILES)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

install: bluepulse
	install bluepulse /usr/local/bin

clean:
	$(RM) $(OJB_FILES) bluepulse config.h ccan/configurator

.PHONY: all install clean

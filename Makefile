VERSION = 0.03

CFLAGS := -Wall -g -O0
LFLAGS :=

LIBTELNET_CFLAGS := $(shell pkg-config libtelnet --cflags)
LIBTELNET_LFLAGS := $(shell pkg-config libtelnet --libs)

CURSES_CFLAGS :=
CURSES_LFLAGS := -lcurses

CLC_CONFIG := -DCLC_VERSION='"$(VERSION)"'

all: clclite

clclite.o: clclite.c
	$(CC) $(CLC_CONFIG) $(LIBTELNET_CFLAGS) $(CURSES_CFLAGS) $(CFLAGS) -c -o $@ $<

clclite: clclite.o
	$(CC) -o $@ $< $(LIBTELNET_LFLAGS) $(CURSES_LFLAGS) $(LFLAGS)

clean:
	rm -f clclite clclite.o 

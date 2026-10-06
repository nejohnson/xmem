CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lX11

all: xmem

xmem: xmem.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f xmem

.PHONY: all clean

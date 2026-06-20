CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
LDFLAGS ?=

SRCS = demo_hwmon.c nanotui.c

all: demo

demo: $(SRCS) nanotui.h
	$(CC) $(CFLAGS) -o demo $(SRCS) $(LDFLAGS)

# static link gets rid of the shared-libc pages that otherwise show up
# in this process's RSS even though they're not really "ours" - see
# README for why that matters for the <1MB claim
static: $(SRCS) nanotui.h
	$(CC) $(CFLAGS) -static -o demo_static $(SRCS) $(LDFLAGS)

clean:
	rm -f demo demo_static *.o

.PHONY: all static clean

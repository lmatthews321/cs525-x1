CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra

.PHONY: all clean

all: pageload

pageload: pageload.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f pageload

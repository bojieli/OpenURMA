# Cross-compile aarch64 SE-mode workloads for the gem5 CHI experiment.
CC      ?= aarch64-linux-gnu-gcc
CFLAGS  ?= -O2 -static -Wall

BINS = ptr_chase shared_write

all: $(BINS)

ptr_chase: ptr_chase.c
	$(CC) $(CFLAGS) -o $@ $<

shared_write: shared_write.c
	$(CC) $(CFLAGS) -lpthread -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean

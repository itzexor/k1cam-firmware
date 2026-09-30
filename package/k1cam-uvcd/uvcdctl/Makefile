CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11

.PHONY: all clean install

all: uvcdctl

uvcdctl: uvcdctl.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

install: uvcdctl
	install -Dm755 uvcdctl $(DESTDIR)/usr/bin/uvcdctl

clean:
	rm -f uvcdctl

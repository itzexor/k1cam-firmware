CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -D_GNU_SOURCE
SRCS = uvcdctl.c firmware.c ../src/fw_bundle.c ../src/fw_crc.c ../src/fw_sha256.c

.PHONY: all clean install

all: uvcdctl

uvcdctl: $(SRCS) uvcdctl.h ../src/uvcd_firmware.h ../src/fw_bundle.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

install: uvcdctl
	install -Dm755 uvcdctl $(DESTDIR)/usr/bin/uvcdctl

clean:
	rm -f uvcdctl

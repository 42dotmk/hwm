.POSIX:

CC      = cc

# Version derived from `git describe` at build time so the binary reports
# the exact tag/commit it was built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c99 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DHWM_VERSION='"$(VERSION)"' -isystem vendor
LDLIBS  = -lX11 -lXrandr -linput -ludev
BINDIR  = $(HOME)/.local/bin

all: hwm

hwm: hwm.o config.o
	$(CC) -o $@ hwm.o config.o $(LDLIBS)

hwm.o: hwm.c hwm.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -c hwm.c

config.o: config.c hwm.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -c config.c

install: hwm
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hwm" $(BINDIR)/hwm

uninstall:
	rm -f $(BINDIR)/hwm

clean:
	rm -f hwm hwm.o config.o

.PHONY: all install uninstall clean

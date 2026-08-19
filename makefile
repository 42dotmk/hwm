.POSIX:

CC      = cc

# Version derived from `git describe` at build time so the binary reports
# the exact tag/commit it was built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DHWM_VERSION='"$(VERSION)"' -isystem vendor
LDLIBS  = -lX11 -lXrandr -linput -ludev
BINDIR  = $(HOME)/.local/bin

all: hwm

hwm: hwm.c hwm.h config.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -o $@ hwm.c $(LDLIBS)

install: hwm
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hwm" $(BINDIR)/hwm

uninstall:
	rm -f $(BINDIR)/hwm

clean:
	rm -f hwm

.PHONY: all install uninstall clean

.POSIX:

CC      = cc

# Version derived from `git describe` at build time so the binary reports
# the exact tag/commit it was built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DHWM_VERSION='"$(VERSION)"' -isystem vendor
LDLIBS  = -lX11 -lXrandr -linput -ludev
BINDIR  = $(HOME)/.local/bin
OBJ     = hwm.o layout.o

all: hwm

hwm: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

hwm.o: hwm.c hwm.h layout.h config.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -c hwm.c

layout.o: layout.c layout.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -c layout.c

# the layout core is X-free: its tests run headless
test_layout: test_layout.o layout.o
	$(CC) -o $@ test_layout.o layout.o

test_layout.o: test_layout.c layout.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -c test_layout.c

check: test_layout
	./test_layout

install: hwm
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hwm" $(BINDIR)/hwm

uninstall:
	rm -f $(BINDIR)/hwm

clean:
	rm -f hwm test_layout $(OBJ) test_layout.o

.PHONY: all check install uninstall clean

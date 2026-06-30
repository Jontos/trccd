target = trccd
packages = libusb-1.0
objects = main.o

prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
libdir ?= $(exec_prefix)/lib
sysconfdir ?= $(prefix)/etc

EXTRA_CFLAGS = -Wall -Wextra -std=gnu23 -pthread -D_GNU_SOURCE $(shell pkg-config --cflags $(packages))
CFLAGS = -O2 $(EXTRA_CFLAGS)
LDLIBS = -pthread $(shell pkg-config --libs $(packages))

.PHONY: all clean debug install

all: $(target)

debug: CFLAGS = -Og -g $(EXTRA_CFLAGS)
debug: $(target)

$(target): $(objects)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(objects):

install: $(target)
	install -Dm755 $(target) $(DESTDIR)$(bindir)/$(target)

	install -Dm644 trccd.service $(DESTDIR)$(libdir)/systemd/system/trccd.service
	install -Dm644 trccd.rules $(DESTDIR)$(libdir)/udev/rules.d/99-trccd.rules
	install -Dm644 trccd.sysusers $(DESTDIR)$(libdir)/sysusers.d/trccd.conf
	install -Dm644 trccd.conf $(DESTDIR)$(sysconfdir)/trccd/trccd.conf

clean:
	rm -f $(target) $(objects)

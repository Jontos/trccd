target = trccd
packages = libusb-1.0
objects = main.o

prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
libdir ?= $(exec_prefix)/lib

CFLAGS = -O2
LDLIBS = $(shell pkgconf --libs $(packages)) -lm 

.PHONY: all clean
all: $(target)

debug: CFLAGS = -Og -g
debug: $(target)

$(target): $(objects)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(objects):

install: $(target)
	install -Dm755 $(target) $(DESTDIR)$(bindir)/$(target)

	install -Dm644 trccd.service $(DESTDIR)$(libdir)/systemd/system/trccd.service
	install -Dm644 99-trccd.rules $(DESTDIR)$(libdir)/udev/rules.d/99-trccd.rules
	install -Dm644 trccd.sysusers $(DESTDIR)$(libdir)/sysusers.d/trccd.conf

clean:
	rm $(target) $(objects)

target = trccd
packages = libusb-1.0

src_dir = src
srcs = $(wildcard $(src_dir)/*.c)
build_dir = build
objs = $(srcs:$(src_dir)/%.c=$(build_dir)/%.o)
deps = $(objs:.o=.d)

prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
libdir ?= $(exec_prefix)/lib
sysconfdir ?= $(prefix)/etc

warnings = -Wall -Wextra
CFLAGS ?= -O2 $(warnings)
ALL_CFLAGS = -std=gnu23 -pthread $(shell pkg-config --cflags $(packages)) $(CFLAGS)
CPPFLAGS += -MMD -MP -D_GNU_SOURCE
LDFLAGS += -pthread
LDLIBS += $(shell pkg-config --libs $(packages))

.PHONY: all clean debug install
.DELETE_ON_ERROR:

all: $(build_dir)/$(target)

debug: CFLAGS = -Og -g $(warnings)
debug: $(build_dir)/$(target)

$(build_dir)/$(target): $(objs)
	$(CC) $(LDFLAGS) $(ALL_CFLAGS) $^ -o $@ $(LDLIBS)

$(build_dir)/%.o: $(src_dir)/%.c | $(build_dir)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c $< -o $@

$(build_dir):
	mkdir -p $@

-include $(deps)

install: $(build_dir)/$(target)
	install -Dm755 $(build_dir)/$(target) $(DESTDIR)$(bindir)/$(target)

	install -Dm644 trccd.service $(DESTDIR)$(libdir)/systemd/system/trccd.service
	install -Dm644 trccd.rules $(DESTDIR)$(libdir)/udev/rules.d/99-trccd.rules
	install -Dm644 trccd.sysusers $(DESTDIR)$(libdir)/sysusers.d/trccd.conf
	install -Dm644 trccd.conf $(DESTDIR)$(sysconfdir)/trccd/trccd.conf

clean:
	rm -rf $(build_dir)

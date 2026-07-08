target = trccd
packages = libusb-1.0

config ?= release
src_dir = src
build_root = build
build_dir = $(build_root)/$(config)

srcs = $(wildcard $(src_dir)/*.c)
objs = $(srcs:$(src_dir)/%.c=$(build_dir)/%.o)
deps = $(objs:.o=.d)

test_dir = tests
test_srcs = $(wildcard $(test_dir)/*.c)
test_bins = $(test_srcs:$(test_dir)/%.c=$(build_dir)/%)

prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
libdir ?= $(exec_prefix)/lib
sysconfdir ?= $(prefix)/etc

warnings = -Wall -Wextra

ifeq ($(config),debug)
CFLAGS = -Og -g $(warnings)
sanitize = -fsanitize=address,undefined
else
CFLAGS ?= -O2 $(warnings)
endif

ALL_CFLAGS = -std=gnu23 -pthread $(shell pkg-config --cflags $(packages)) $(sanitize) $(CFLAGS)
CPPFLAGS += -MMD -MP -D_GNU_SOURCE
LDFLAGS += -pthread $(sanitize)
LDLIBS += $(shell pkg-config --libs $(packages))

.PHONY: all debug check run-checks install clean
.DELETE_ON_ERROR:

all: $(build_dir)/$(target)

debug:
	@$(MAKE) config=debug

check:
	@$(MAKE) config=debug run-checks

run-checks: $(test_bins)
	@for t in $^; do ./$$t || exit 1; done

$(build_dir)/$(target): $(objs)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(build_dir)/test_%: $(test_dir)/test_%.c $(build_dir)/%.o | $(build_dir)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) $^ -o $@

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
	rm -rf $(build_root)

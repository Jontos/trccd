target = trccd
packages = libusb-1.0
objects = main.o

CFLAGS = -O2
LDLIBS = $(shell pkgconf --libs $(packages)) -lm 

.PHONY: all clean
all: $(target)

debug: CFLAGS = -Og -g
debug: $(target)

$(target): $(objects)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(objects):

clean:
	rm $(target) $(objects)

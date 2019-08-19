CC ?= gcc
LD ?= gcc
CFLAGS := ${CFLAGS}
CFLAGS += -march=native -O2 -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wpedantic

CFLAGS += $(shell pkg-config --cflags xcb-util xcb-image xcb-renderutil xcb-xrm fontconfig pangoft2)
LDLIBS := $(shell pkg-config --libs xcb-util xcb-image xcb-renderutil xcb-xrm fontconfig pangoft2) -lm -lrt

xpanel: xpanel.o

.PHONY: clean
clean:
	-rm xpanel xpanel.o

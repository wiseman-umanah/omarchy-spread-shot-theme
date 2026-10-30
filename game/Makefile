# Spread Shot: the playable wallpaper that ships with the Spread Shot Omarchy theme.
#
#   make             build ./spread-shot
#   make check       headless gameplay tests
#   make install     install the game and wire it into Omarchy (hooks + Super+Ctrl+G)
#   make uninstall   remove everything `make install` added
#   make art         re-render the theme's backgrounds and unlock images
#   make trailer     render the README GIF and a gameplay trailer video

VERSION ?= 1.0.0
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

BUILD := build
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
PROTO_H := $(BUILD)/wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_C := $(BUILD)/wlr-layer-shell-unstable-v1-protocol.c $(BUILD)/xdg-shell-protocol.c
OBJ := $(SRC:src/%.c=$(BUILD)/%.o) $(PROTO_C:.c=.o)

override CPPFLAGS += -Isrc -I$(BUILD) -DVERSION='"$(VERSION)"'
override CFLAGS += $(shell $(PKG_CONFIG) --cflags wayland-client)
LDLIBS += $(shell $(PKG_CONFIG) --libs wayland-client) -lm

all: spread-shot

spread-shot: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/%.o: src/%.c $(HDR) $(PROTO_H) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(BUILD)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(PROTO_H): protocol/wlr-layer-shell-unstable-v1.xml | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@

$(BUILD)/wlr-layer-shell-unstable-v1-protocol.c: protocol/wlr-layer-shell-unstable-v1.xml | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

# The layer-shell protocol references xdg_popup, so its interface must be linked in.
$(BUILD)/xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD):
	mkdir -p $@

check: spread-shot
	tests/check.sh ./spread-shot

install: spread-shot
	BINDIR="$(BINDIR)" scripts/install.sh ./spread-shot

uninstall:
	BINDIR="$(BINDIR)" scripts/uninstall.sh

art: spread-shot
	scripts/render-art.sh ./spread-shot ..

trailer: spread-shot | $(BUILD)
	scripts/render-trailer.sh ./spread-shot $(BUILD)/spread-shot-trailer.mp4 ../docs/spread-shot.gif

clean:
	rm -rf $(BUILD) spread-shot

.PHONY: all check install uninstall art trailer clean

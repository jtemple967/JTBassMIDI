NAME   = JTBassMIDI
BUNDLE = $(NAME).lv2

CC     ?= cc
CFLAGS ?= -O2
override CFLAGS += -std=c99 -Wall -Wextra -fvisibility=hidden
override CFLAGS += $(shell pkg-config --cflags lv2 2>/dev/null)
LDLIBS  = -lm

ifeq ($(OS),Windows_NT)
  LIB_EXT     = .dll
  LDFLAGS    += -shared -static-libgcc
  INSTALL_DIR ?= $(APPDATA)/LV2
else
  UNAME := $(shell uname -s)
  ifeq ($(UNAME),Darwin)
    LIB_EXT     = .dylib
    LDFLAGS    += -dynamiclib
    INSTALL_DIR ?= $(HOME)/Library/Audio/Plug-Ins/LV2
  else
    LIB_EXT     = .so
    override CFLAGS += -fPIC
    LDFLAGS    += -shared -Wl,--no-undefined
    INSTALL_DIR ?= $(HOME)/.lv2
  endif
endif

all: $(BUNDLE)/$(NAME)$(LIB_EXT) $(BUNDLE)/manifest.ttl $(BUNDLE)/$(NAME).ttl modgui-bundle

$(BUNDLE):
	mkdir -p $(BUNDLE)

$(BUNDLE)/$(NAME)$(LIB_EXT): $(NAME).c | $(BUNDLE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUNDLE)/manifest.ttl: manifest.ttl.in | $(BUNDLE)
	sed 's/@LIB_EXT@/$(LIB_EXT)/' $< > $@

$(BUNDLE)/$(NAME).ttl: $(NAME).ttl | $(BUNDLE)
	cp $< $@

modgui-bundle: modgui.ttl $(wildcard modgui/*) | $(BUNDLE)
	cp modgui.ttl $(BUNDLE)/
	mkdir -p $(BUNDLE)/modgui
	cp modgui/* $(BUNDLE)/modgui/

# Regenerate the GUI images (needs Python 3 + Pillow)
modgui-images:
	python3 tools/make_modgui.py

# Offline tracking test: synthetic plucks in, MIDI out, latency report
test: tools/test_tracking
	./tools/test_tracking

tools/test_tracking: tools/test_tracking.c $(NAME).c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: all
	mkdir -p "$(INSTALL_DIR)/$(BUNDLE)"
	cp -r $(BUNDLE)/* "$(INSTALL_DIR)/$(BUNDLE)/"

uninstall:
	rm -rf "$(INSTALL_DIR)/$(BUNDLE)"

clean:
	rm -rf $(BUNDLE) tools/test_tracking

.PHONY: all install uninstall clean test modgui-bundle modgui-images

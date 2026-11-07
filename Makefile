# Flywheel Lite: a Flywheel bytecode VM. Needs only a C11 compiler and libm.
#   make            release build in build/flywheel-lite
#   make debug      debug build with sanitizers in build-debug/flywheel-lite
#   make test       run the conformance and Flywheel Lite test suites
#   make install    install flywheel-lite and its short alias fwl to $(PREFIX)/bin (default /usr/local)
# Needs a Flywheel checkout (github.com/fwlang/flywheel) for the stdlib and conformance suite:
# FLYWHEEL_DIR defaults to .. (flywheel-lite cloned inside the flywheel checkout).

CC      ?= cc
CFLAGS  ?= -O2
PREFIX  ?= /usr/local
BUILD   ?= build
FLYWHEEL_DIR ?= ..
STDLIB  ?= $(FLYWHEEL_DIR)/stdlib/fw

WARN    := -Wall -Wextra -Wno-unused-parameter
SOURCES := $(wildcard src/*.c)
HEADERS := $(wildcard src/*.h)
LIBMODS := csv url log args bigint decimal random
LIBSRC  := $(foreach m,$(LIBMODS),$(STDLIB)/$(m).fw)

all: $(BUILD)/flywheel-lite

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/embed: tools/embed.c | $(BUILD)
	$(CC) -O1 -o $@ $<

$(BUILD)/stdlib_embed.c: $(BUILD)/embed $(LIBSRC)
	$(BUILD)/embed $@ $(foreach m,$(LIBMODS),$(m)=$(STDLIB)/$(m).fw)

$(BUILD)/flywheel-lite: $(SOURCES) $(HEADERS) $(BUILD)/stdlib_embed.c
	$(CC) -std=c11 $(CFLAGS) $(WARN) -Isrc -DLITE_TEST_DIR='"$(abspath tests)"' -o $@ $(SOURCES) $(BUILD)/stdlib_embed.c -lm

debug:
	$(MAKE) BUILD=build-debug CFLAGS="-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

test: $(BUILD)/flywheel-lite
	FLYWHEEL_DIR=$(FLYWHEEL_DIR) python3 tests/run_tests.py $(BUILD)/flywheel-lite

install: $(BUILD)/flywheel-lite
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BUILD)/flywheel-lite $(DESTDIR)$(PREFIX)/bin/flywheel-lite
	ln -sf flywheel-lite $(DESTDIR)$(PREFIX)/bin/fwl

clean:
	rm -rf build build-debug

.PHONY: all debug test install clean

# Turbine: a Flywheel bytecode VM. Needs only a C11 compiler and libm.
#   make            release build in build/turbine
#   make debug      debug build with sanitizers in build-debug/turbine
#   make test       run the conformance and Turbine test suites
#   make install    install to $(PREFIX)/bin (default /usr/local)
# Needs a Flywheel checkout (github.com/fwlang/flywheel) for the stdlib and conformance suite:
# FLYWHEEL_DIR defaults to .. (turbine cloned inside the flywheel checkout).

CC      ?= cc
CFLAGS  ?= -O2
PREFIX  ?= /usr/local
BUILD   ?= build
FLYWHEEL_DIR ?= ..
STDLIB  ?= $(FLYWHEEL_DIR)/stdlib/fw

WARN    := -Wall -Wextra -Wno-unused-parameter
SOURCES := $(wildcard src/*.c)
HEADERS := $(wildcard src/*.h)
LIBMODS := csv url log args bigint decimal
LIBSRC  := $(foreach m,$(LIBMODS),$(STDLIB)/$(m).fw)

all: $(BUILD)/turbine

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/embed: tools/embed.c | $(BUILD)
	$(CC) -O1 -o $@ $<

$(BUILD)/stdlib_embed.c: $(BUILD)/embed $(LIBSRC)
	$(BUILD)/embed $@ $(foreach m,$(LIBMODS),$(m)=$(STDLIB)/$(m).fw)

$(BUILD)/turbine: $(SOURCES) $(HEADERS) $(BUILD)/stdlib_embed.c
	$(CC) -std=c11 $(CFLAGS) $(WARN) -Isrc -o $@ $(SOURCES) $(BUILD)/stdlib_embed.c -lm

debug:
	$(MAKE) BUILD=build-debug CFLAGS="-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

test: $(BUILD)/turbine
	FLYWHEEL_DIR=$(FLYWHEEL_DIR) python3 tests/run_tests.py $(BUILD)/turbine

install: $(BUILD)/turbine
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BUILD)/turbine $(DESTDIR)$(PREFIX)/bin/turbine

clean:
	rm -rf build build-debug

.PHONY: all debug test install clean

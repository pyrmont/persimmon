.POSIX:

CC = cc
AR = ar
INSTALL = install

CPPFLAGS =
CFLAGS = -O2 -std=c99 -Wall -Wextra
LDFLAGS =
LDLIBS =
ARFLAGS = rcs

PREFIX = /usr/local
DESTDIR =
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

BUILD_DIR = _build/core
AMAL_DIR = _build/amalgam
LIBRARY = $(BUILD_DIR)/libpersimmon.a
TEST = $(BUILD_DIR)/persimmon-core-test
EXAMPLE = $(BUILD_DIR)/persimmon-example

SOURCES = \
	src/persimmon.c \
	src/persimmon_vector.c \
	src/persimmon_list.c \
	src/persimmon_hamt.c \
	src/persimmon_map.c \
	src/persimmon_set.c

OBJECTS = \
	$(BUILD_DIR)/persimmon.o \
	$(BUILD_DIR)/persimmon_vector.o \
	$(BUILD_DIR)/persimmon_list.o \
	$(BUILD_DIR)/persimmon_hamt.o \
	$(BUILD_DIR)/persimmon_map.o \
	$(BUILD_DIR)/persimmon_set.o

all: $(LIBRARY)

help:
	@printf '%s\n' \
		'Targets:' \
		'  all           build the core static library (default)' \
		'  check         compile and run the core C tests' \
		'  example       build the C example' \
		'  amalgamation  write the core as a single .c and .h pair' \
		'  install       install the library and public header' \
		'  clean         remove the core build products' \
		'  help          show this help' \
		'' \
		'Common overrides:' \
		'  CC AR CPPFLAGS CFLAGS LDFLAGS LDLIBS' \
		'  PREFIX DESTDIR LIBDIR INCLUDEDIR'

$(BUILD_DIR):
	mkdir -p "$(BUILD_DIR)"

$(BUILD_DIR)/persimmon.o: src/persimmon.c src/persimmon_internal.h include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/persimmon.c -o $@

$(BUILD_DIR)/persimmon_vector.o: src/persimmon_vector.c src/persimmon_internal.h include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/persimmon_vector.c -o $@

$(BUILD_DIR)/persimmon_list.o: src/persimmon_list.c src/persimmon_internal.h include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/persimmon_list.c -o $@

$(BUILD_DIR)/persimmon_hamt.o: src/persimmon_hamt.c src/persimmon_internal.h include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/persimmon_hamt.c -o $@

$(BUILD_DIR)/persimmon_map.o: src/persimmon_map.c src/persimmon_internal.h include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/persimmon_map.c -o $@

$(BUILD_DIR)/persimmon_set.o: src/persimmon_set.c src/persimmon_internal.h include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/persimmon_set.c -o $@

$(LIBRARY): $(OBJECTS)
	$(AR) $(ARFLAGS) $@ $(OBJECTS)

$(TEST): test/core.c $(SOURCES) src/persimmon_internal.h include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPERSIMM_TEST_ALLOC -Iinclude \
		test/core.c $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

check: $(TEST)
	$(TEST)

$(EXAMPLE): res/examples/core.c $(LIBRARY) include/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude res/examples/core.c $(LIBRARY) \
		$(LDFLAGS) $(LDLIBS) -o $@

example: $(EXAMPLE)

$(AMAL_DIR):
	mkdir -p "$(AMAL_DIR)"

# One translation unit for anyone vendoring the core rather than linking it.
# Only the quoted includes come out: the angled ones are either idempotent, as
# the standard headers are, or sit inside the refcount conditionals in
# persimmon_internal.h and have to stay where they are.
#
# Written every time rather than when the sources look newer. Make compares
# modification times a whole second at a time, so a source edited in the same
# second as the last run would be missed, and a stale amalgamation that still
# compiles is worse than the cost of rewriting two files that take milliseconds
# to produce.
amalgamation: $(AMAL_DIR)
	cp include/persimmon.h "$(AMAL_DIR)/persimmon.h"
	{ echo '#include "persimmon.h"'; \
		grep -v '^#include "' src/persimmon_internal.h; \
		for f in $(SOURCES); do grep -v '^#include "' "$$f"; done; \
	} > "$(AMAL_DIR)/persimmon.c"

install: $(LIBRARY)
	$(INSTALL) -d "$(DESTDIR)$(LIBDIR)" "$(DESTDIR)$(INCLUDEDIR)"
	$(INSTALL) -m 644 "$(LIBRARY)" "$(DESTDIR)$(LIBDIR)/libpersimmon.a"
	$(INSTALL) -m 644 include/persimmon.h "$(DESTDIR)$(INCLUDEDIR)/persimmon.h"

clean:
	@case "$(BUILD_DIR)" in _build/*) ;; *) \
		echo "refusing to remove BUILD_DIR outside _build" >&2; exit 1;; esac
	@case "$(AMAL_DIR)" in _build/*) ;; *) \
		echo "refusing to remove AMAL_DIR outside _build" >&2; exit 1;; esac
	rm -rf "$(BUILD_DIR)" "$(AMAL_DIR)"

.PHONY: all help check example amalgamation install clean

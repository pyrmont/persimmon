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
		'  all      build the core static library (default)' \
		'  check    compile and run the core C tests' \
		'  example  build the C example' \
		'  install  install the library and public header' \
		'  clean    remove the core build products' \
		'  help     show this help' \
		'' \
		'Common overrides:' \
		'  CC AR CPPFLAGS CFLAGS LDFLAGS LDLIBS' \
		'  PREFIX DESTDIR LIBDIR INCLUDEDIR'

$(BUILD_DIR):
	mkdir -p "$(BUILD_DIR)"

$(BUILD_DIR)/persimmon.o: src/persimmon.c src/persimmon_internal.h inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinc -c src/persimmon.c -o $@

$(BUILD_DIR)/persimmon_vector.o: src/persimmon_vector.c src/persimmon_internal.h inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinc -c src/persimmon_vector.c -o $@

$(BUILD_DIR)/persimmon_list.o: src/persimmon_list.c src/persimmon_internal.h inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinc -c src/persimmon_list.c -o $@

$(BUILD_DIR)/persimmon_hamt.o: src/persimmon_hamt.c src/persimmon_internal.h inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinc -c src/persimmon_hamt.c -o $@

$(BUILD_DIR)/persimmon_map.o: src/persimmon_map.c src/persimmon_internal.h inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinc -c src/persimmon_map.c -o $@

$(BUILD_DIR)/persimmon_set.o: src/persimmon_set.c src/persimmon_internal.h inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinc -c src/persimmon_set.c -o $@

$(LIBRARY): $(OBJECTS)
	$(AR) $(ARFLAGS) $@ $(OBJECTS)

$(TEST): test/core.c $(SOURCES) src/persimmon_internal.h inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPERSIMM_TEST_ALLOC -Iinc \
		test/core.c $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

check: $(TEST)
	$(TEST)

$(EXAMPLE): res/examples/core.c $(LIBRARY) inc/persimmon.h $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinc res/examples/core.c $(LIBRARY) \
		$(LDFLAGS) $(LDLIBS) -o $@

example: $(EXAMPLE)

install: $(LIBRARY)
	$(INSTALL) -d "$(DESTDIR)$(LIBDIR)" "$(DESTDIR)$(INCLUDEDIR)"
	$(INSTALL) -m 644 "$(LIBRARY)" "$(DESTDIR)$(LIBDIR)/libpersimmon.a"
	$(INSTALL) -m 644 inc/persimmon.h "$(DESTDIR)$(INCLUDEDIR)/persimmon.h"

clean:
	@case "$(BUILD_DIR)" in _build/*) ;; *) \
		echo "refusing to remove BUILD_DIR outside _build" >&2; exit 1;; esac
	rm -rf "$(BUILD_DIR)"

.PHONY: all help check example install clean

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: all check clean
all: build/init build/bbpkg

build:
	mkdir -p $@

build/init: init.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< $(LDFLAGS) -o $@

build/bbpkg: bbpkg.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< $(LDFLAGS) -o $@

check: all
	bash -n build-bbos-toolchain.sh
	python3 tests/test_bbpkg.py

clean:
	rm -rf build

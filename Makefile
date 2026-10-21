CC        ?= cc
CFLAGS    ?= -O2 -g
FK_CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wformat
PREFIX    ?= /usr/local
BUILD     ?= build

# Two builds, not one -fsanitize=address,undefined: in a combined build, UBSan
# ignores log_path and reports only to stderr, which a detached supervisor
# sends to a focker.log that the suite deletes.
ASAN_CFLAGS  := -O1 -g -fno-omit-frame-pointer -fsanitize=address
UBSAN_CFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=all
LINT_C     := $(wildcard src/*.c) tests/unit.c $(wildcard examples/*.c)
LINT_SH    := tests/run.sh tests/sanitize.sh examples/demo.sh

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:src/%.c=$(BUILD)/%.o)
EXAMPLES := $(BUILD)/ticker $(BUILD)/oldparent $(BUILD)/newparent

.PHONY: all examples unit test asan lint ci demo install uninstall clean

all: $(BUILD)/focker examples

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c src/focker.h | $(BUILD)
	$(CC) $(CFLAGS) $(FK_CFLAGS) -c -o $@ $<

$(BUILD)/focker: $(OBJ)
	$(CC) $(CFLAGS) $(FK_CFLAGS) -o $@ $(OBJ)

examples: $(EXAMPLES)

$(BUILD)/%: examples/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(FK_CFLAGS) -pthread -o $@ $<

$(BUILD)/unit: tests/unit.c $(BUILD)/procfs.o $(BUILD)/util.o $(BUILD)/unixdiag.o src/focker.h
	$(CC) $(CFLAGS) $(FK_CFLAGS) -o $@ tests/unit.c $(BUILD)/procfs.o $(BUILD)/util.o $(BUILD)/unixdiag.o

unit: $(BUILD)/unit
	$(BUILD)/unit

test: all unit
	FOCKER_BUILD=$(BUILD) tests/run.sh

# ASan (with LSan) in build-asan/, then UBSan in build-ubsan/; each runs both
# suites, and any sanitizer report fails
asan:
	rm -rf build-asan build-ubsan   # make can't tell objects built with other flags
	$(MAKE) BUILD=build-asan CFLAGS="$(ASAN_CFLAGS)" all build-asan/unit
	tests/sanitize.sh build-asan
	$(MAKE) BUILD=build-ubsan CFLAGS="$(UBSAN_CFLAGS)" all build-ubsan/unit
	tests/sanitize.sh build-ubsan

# gcc's static analyzer (warnings are errors) and shellcheck
lint:
	@for f in $(LINT_C); do \
	    echo "analyze $$f"; \
	    $(CC) $(FK_CFLAGS) -O2 -fanalyzer -Werror -c -o /dev/null $$f || exit 1; \
	done
	shellcheck -s bash $(LINT_SH)

# what ci/github-actions.yml runs
ci: lint test asan

demo: all
	examples/demo.sh

install: $(BUILD)/focker
	install -Dm755 $(BUILD)/focker $(DESTDIR)$(PREFIX)/bin/focker

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/focker

clean:
	rm -rf build build-asan build-ubsan

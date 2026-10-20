CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wformat
PREFIX  ?= /usr/local
BUILD   := build

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:src/%.c=$(BUILD)/%.o)
EXAMPLES := $(BUILD)/ticker $(BUILD)/oldparent $(BUILD)/newparent

.PHONY: all examples test demo install uninstall clean

all: $(BUILD)/focker examples

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c src/focker.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/focker: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

examples: $(EXAMPLES)

$(BUILD)/%: examples/%.c | $(BUILD)
	$(CC) $(CFLAGS) -pthread -o $@ $<

test: all
	tests/run.sh

demo: all
	examples/demo.sh

install: $(BUILD)/focker
	install -Dm755 $(BUILD)/focker $(DESTDIR)$(PREFIX)/bin/focker

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/focker

clean:
	rm -rf $(BUILD)

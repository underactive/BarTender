# firmware/test/config_store/Makefile
#
# Build and run the pure-C LRU unit-tests (no ESP-IDF, no NVS).
#
# Usage:
#   make          — compile + run
#   make build    — compile only
#   make run      — run existing binary
#   make clean    — remove build artefacts

CC      ?= cc
CFLAGS  := -I../../main -Wall -Wextra -Wpedantic -O0 -g -std=c11
TARGET  := runtests
SRC     := test_wifi_lru.c

.PHONY: all build run clean

all: build run

build: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

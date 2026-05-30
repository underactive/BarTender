# Host-compiled unit test for firmware/main/stats_model.c
# Compiles the REAL stats_model.c against vendored cJSON and a no-op esp_log shim.
# Usage: cd firmware/test/stats_model && make

CC      ?= cc
CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O0 -g \
           -I./shim \
           -I./vendor \
           -I../../main

SRCS    := ../../main/stats_model.c \
           vendor/cJSON.c \
           test_stats_model.c

TARGET  := runtests

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

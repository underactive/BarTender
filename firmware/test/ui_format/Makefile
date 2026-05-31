# Host-compiled unit test for firmware/main/ui_format.c
# Compiles the REAL ui_format.c against a minimal lvgl/freertos shim. The pure
# helpers under test touch no LVGL widget globals or shared `st`, so ui_format.c
# links standalone (no other main/*.c needed).
# Usage: cd firmware/test/ui_format && make run

CC      ?= cc
CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O0 -g \
           -I./shim \
           -I../../main

SRCS    := ../../main/ui_format.c \
           test_ui_format.c

TARGET  := runtests

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

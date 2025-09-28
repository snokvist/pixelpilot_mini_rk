CC ?= gcc
CFLAGS ?= -O2 -Wall
CFLAGS += -Iinclude
CFLAGS += $(shell pkg-config --cflags libdrm libudev)
LDFLAGS += $(shell pkg-config --libs libdrm libudev)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
TARGET := pixelpilot_mini_rk

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean

CC ?= gcc
PKG_CONFIG ?= pkg-config
PKG_DRMCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags libdrm libudev)
PKG_DRMLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs libdrm libudev)

CFLAGS ?= -O2 -Wall
CFLAGS += -Iinclude
ifeq ($(strip $(PKG_DRMCFLAGS)),)
CFLAGS += -I/usr/include/libdrm
else
CFLAGS += $(PKG_DRMCFLAGS)
endif

ifneq ($(strip $(PKG_DRMLIBS)),)
LDFLAGS += $(PKG_DRMLIBS)
else
LDFLAGS += -ldrm -ludev
endif

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

CC ?= gcc
PKG_CONFIG ?= pkg-config
PKG_DRMCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags libdrm libudev)
PKG_DRMLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs libdrm libudev)
PKG_GSTCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
PKG_GSTLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
PKG_MPPCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags rockchip-mpp)
PKG_MPPLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs rockchip-mpp)

CFLAGS ?= -O2 -Wall
CFLAGS += -Iinclude
ifeq ($(strip $(PKG_DRMCFLAGS)),)
CFLAGS += -I/usr/include/libdrm
else
CFLAGS += $(PKG_DRMCFLAGS)
endif

ifneq ($(strip $(PKG_GSTCFLAGS)),)
CFLAGS += $(PKG_GSTCFLAGS)
endif

ifneq ($(strip $(PKG_MPPCFLAGS)),)
CFLAGS += $(PKG_MPPCFLAGS)
else
CFLAGS += -I/usr/include/rockchip
endif

ifneq ($(strip $(PKG_DRMLIBS)),)
LDFLAGS += $(PKG_DRMLIBS)
else
LDFLAGS += -ldrm -ludev
endif

ifneq ($(strip $(PKG_GSTLIBS)),)
LDFLAGS += $(PKG_GSTLIBS)
else
LDFLAGS += -lgstreamer-1.0 -lgstvideo-1.0 -lgstapp-1.0 -lgobject-2.0 -lglib-2.0
endif

ifneq ($(strip $(PKG_MPPLIBS)),)
LDFLAGS += $(PKG_MPPLIBS)
else
LDFLAGS += -lrockchip_mpp
endif

LDFLAGS += -lpthread

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

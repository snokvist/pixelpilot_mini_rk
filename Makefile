CC ?= gcc
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
SYSCONFDIR ?= /etc
ASSETDIR := $(PREFIX)/share/pixelpilot_mini_rk
SYSTEMD_DIR := /etc/systemd/system

# Default to using 4 parallel jobs unless the caller already requested a
# specific level of parallelism (e.g. via `make -j8`).
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j4
endif
PKG_CONFIG ?= pkg-config
PKG_DRMCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags libdrm libudev)
PKG_DRMLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs libdrm libudev)
PKG_GSTCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
PKG_GSTLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
PKG_MPPCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags rockchip-mpp)
PKG_MPPLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs rockchip-mpp)
PKG_GLIBCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags glib-2.0)
PKG_RGACFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags librga)
PKG_RGALIBS := $(shell $(PKG_CONFIG) --silence-errors --libs librga)

CFLAGS ?= -O2 -Wall
CFLAGS += -Iinclude -Ithird_party/minimp4

# Allow opt-in control over NEON usage while auto-detecting safe defaults for
# supported ARM toolchains. Users can override by invoking `make ENABLE_NEON=0`
# or `make ENABLE_NEON=1`.
ENABLE_NEON ?= auto
CC_MACHINE := $(strip $(shell $(CC) -dumpmachine 2>/dev/null))
ifeq ($(CC_MACHINE),)
CC_MACHINE := $(strip $(shell uname -m 2>/dev/null))
endif

NEON_CFLAGS :=
NEON_DISABLE_DEFINE :=
NEON_ENABLED := 0

ifeq ($(ENABLE_NEON),0)
NEON_DISABLE_DEFINE := -DPIXELPILOT_DISABLE_NEON
else
  ifeq ($(ENABLE_NEON),1)
NEON_CFLAGS := -mfpu=neon -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
  else ifeq ($(ENABLE_NEON),auto)
    ifneq (,$(findstring aarch64,$(CC_MACHINE)))
NEON_CFLAGS := -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
    endif
    ifneq (,$(findstring arm64,$(CC_MACHINE)))
NEON_CFLAGS := -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
    endif
    ifneq (,$(findstring armv7,$(CC_MACHINE)))
NEON_CFLAGS := -mfpu=neon -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
    endif
  endif
endif

CFLAGS += $(NEON_CFLAGS) $(NEON_DISABLE_DEFINE)
ifeq ($(strip $(PKG_DRMCFLAGS)),)
CFLAGS += -I/usr/include/libdrm
else
CFLAGS += $(PKG_DRMCFLAGS)
endif

ifneq ($(strip $(PKG_GSTCFLAGS)),)
CFLAGS += $(PKG_GSTCFLAGS)
endif

ifneq ($(strip $(PKG_GLIBCFLAGS)),)
CFLAGS += $(PKG_GLIBCFLAGS)
else
CFLAGS += -I/usr/include/glib-2.0 -I/usr/lib/$(shell uname -m)-linux-gnu/glib-2.0/include
endif

ifneq ($(strip $(PKG_MPPCFLAGS)),)
CFLAGS += $(PKG_MPPCFLAGS)
else
CFLAGS += -I/usr/include/rockchip
endif

ifneq ($(strip $(PKG_RGACFLAGS)),)
CFLAGS += $(PKG_RGACFLAGS) -DPIXELPILOT_HAVE_RGA=1
else
CFLAGS += -DPIXELPILOT_HAVE_RGA=0
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

ifneq ($(strip $(PKG_RGALIBS)),)
LDFLAGS += $(PKG_RGALIBS)
endif

LDFLAGS += -lpthread -lm

TARGET := pixelpilot_mini_rk
COMPANION := osd_ext_feed

SRC := $(wildcard src/*.c)
APP_SRC := $(filter-out src/osd_ext_feed.c,$(SRC))
APP_OBJ := $(APP_SRC:.c=.o)
COMPANION_SRC := src/osd_ext_feed.c
COMPANION_OBJ := $(COMPANION_SRC:.c=.o)

COMPANION_LDFLAGS := -lm

all: $(TARGET) $(COMPANION)

tests/video_stabilizer_test: tests/video_stabilizer_test.c src/video_stabilizer.c src/logging.c
	$(CC) $(CFLAGS) tests/video_stabilizer_test.c src/video_stabilizer.c src/logging.c -o $@ $(LDFLAGS)

test: tests/video_stabilizer_test
	@echo "Running video stabilizer smoke test"
	./tests/video_stabilizer_test

$(TARGET): $(APP_OBJ)
	$(CC) $(APP_OBJ) -o $@ $(LDFLAGS)

$(COMPANION): $(COMPANION_OBJ)
	$(CC) $(CFLAGS) $(COMPANION_OBJ) -o $@ $(COMPANION_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(APP_OBJ) $(COMPANION_OBJ) $(TARGET) $(COMPANION) tests/video_stabilizer_test

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 0755 $(COMPANION) $(DESTDIR)$(BINDIR)/$(COMPANION)
	ln -sf $(COMPANION) $(DESTDIR)$(BINDIR)/osd_external_feed
	install -d $(DESTDIR)$(SYSCONFDIR)
	sed -e 's|@ASSETDIR@|$(ASSETDIR)|g' config/pixelpilot_mini.ini > $(DESTDIR)$(SYSCONFDIR)/pixelpilot_mini.ini
	chmod 0644 $(DESTDIR)$(SYSCONFDIR)/pixelpilot_mini.ini
	install -d $(DESTDIR)$(SYSTEMD_DIR)
	sed -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@SYSCONFDIR@|$(SYSCONFDIR)|g' systemd/pixelpilot_mini_rk.service > $(DESTDIR)$(SYSTEMD_DIR)/pixelpilot_mini_rk.service
	sed -e 's|@BINDIR@|$(BINDIR)|g' systemd/osd_external_feed.service > $(DESTDIR)$(SYSTEMD_DIR)/osd_external_feed.service
	chmod 0644 $(DESTDIR)$(SYSTEMD_DIR)/pixelpilot_mini_rk.service $(DESTDIR)$(SYSTEMD_DIR)/osd_external_feed.service
	install -d $(DESTDIR)$(ASSETDIR)
	install -m 0644 assets/spinner_ai_1080p30.h265 $(DESTDIR)$(ASSETDIR)/spinner_ai_1080p30.h265
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		systemctl daemon-reload; \
		systemctl enable pixelpilot_mini_rk.service osd_external_feed.service; \
	fi

uninstall:
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		systemctl disable --now osd_external_feed.service 2>/dev/null || true; \
		systemctl disable --now pixelpilot_mini_rk.service 2>/dev/null || true; \
	fi
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(BINDIR)/$(COMPANION) $(DESTDIR)$(BINDIR)/osd_external_feed
	rm -f $(DESTDIR)$(SYSTEMD_DIR)/pixelpilot_mini_rk.service $(DESTDIR)$(SYSTEMD_DIR)/osd_external_feed.service
	rm -f $(DESTDIR)$(SYSCONFDIR)/pixelpilot_mini.ini
	rm -f $(DESTDIR)$(ASSETDIR)/spinner_ai_1080p30.h265
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		systemctl daemon-reload; \
	fi

.PHONY: all clean install uninstall

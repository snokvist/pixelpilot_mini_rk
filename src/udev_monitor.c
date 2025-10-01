#include "udev_monitor.h"
#include "logging.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

int udev_monitor_open(UdevMonitor *um) {
    memset(um, 0, sizeof(*um));
    um->udev = udev_new();
    if (!um->udev) {
        return -1;
    }
    um->mon = udev_monitor_new_from_netlink(um->udev, "udev");
    if (!um->mon) {
        udev_unref(um->udev);
        um->udev = NULL;
        return -1;
    }
    if (udev_monitor_filter_add_match_subsystem_devtype(um->mon, "drm", NULL) < 0) {
        udev_monitor_unref(um->mon);
        um->mon = NULL;
        udev_unref(um->udev);
        um->udev = NULL;
        return -1;
    }
    if (udev_monitor_enable_receiving(um->mon) < 0) {
        udev_monitor_unref(um->mon);
        um->mon = NULL;
        udev_unref(um->udev);
        um->udev = NULL;
        return -1;
    }
    um->fd = udev_monitor_get_fd(um->mon);
    LOGI("udev monitor active (fd=%d)", um->fd);
    return 0;
}

void udev_monitor_close(UdevMonitor *um) {
    if (!um) {
        return;
    }
    if (um->mon) {
        udev_monitor_unref(um->mon);
    }
    if (um->udev) {
        udev_unref(um->udev);
    }
    memset(um, 0, sizeof(*um));
}

int udev_monitor_did_hotplug(UdevMonitor *um) {
    if (!um || !um->mon) {
        return 0;
    }
    struct udev_device *dev = udev_monitor_receive_device(um->mon);
    if (!dev) {
        return 0;
    }
    const char *subsys = udev_device_get_subsystem(dev);
    const char *act = udev_device_get_action(dev);
    const char *sysname = udev_device_get_sysname(dev);
    const char *hotplug = udev_device_get_property_value(dev, "HOTPLUG");
    LOGV("udev: subsys=%s action=%s sys=%s hotplug=%s", subsys ? subsys : "?", act ? act : "?",
         sysname ? sysname : "?", hotplug ? hotplug : "?");

    gboolean is_drm = (subsys != NULL) && (strcmp(subsys, "drm") == 0);
    gboolean action_change = (act != NULL) &&
                             (strcmp(act, "change") == 0 || strcmp(act, "add") == 0 ||
                              strcmp(act, "remove") == 0);
    gboolean flagged_hotplug = (hotplug != NULL) && (strcmp(hotplug, "1") == 0);

    udev_device_unref(dev);

    if (!is_drm || !action_change || !flagged_hotplug) {
        return 0;
    }

    return 1;
}

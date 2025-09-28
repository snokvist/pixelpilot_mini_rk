#ifndef UDEV_MONITOR_H
#define UDEV_MONITOR_H

#include <libudev.h>

typedef struct {
    int fd;
    struct udev *udev;
    struct udev_monitor *mon;
} UdevMonitor;

int udev_monitor_open(UdevMonitor *um);
void udev_monitor_close(UdevMonitor *um);
int udev_monitor_did_hotplug(UdevMonitor *um);

#endif // UDEV_MONITOR_H

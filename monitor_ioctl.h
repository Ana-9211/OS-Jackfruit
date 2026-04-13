#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H
#include <linux/ioctl.h>
#define MONITOR_MAGIC 'M'
struct container_info {
    int           pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
};
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_info)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, int)
#endif

#ifndef PLINK_RESOURCE_LOCK_H
#define PLINK_RESOURCE_LOCK_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

/* Separate open descriptions are intentional: flock must exclude forked clients. */
static inline int plink_resource_lock(const char *resource)
{
    char path[128];
    snprintf(path, sizeof(path), "/run/lock/plink-%s.lock", resource);
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0660);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#endif

/* Test-only Linux interposer for deterministic BUILD publication races. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static _Thread_local int gated;

static void
marker(const char *dir, const char *name)
{
    char path[PATH_MAX];
    int fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        _exit(91);
    close(fd);
}

static void
pause_build(const char *phase, const char *path)
{
    const char *dir = getenv("PGCUVS_BUILD_GATE_DIR");
    char arm[PATH_MAX], want_phase[16], key[32];
    FILE *file;
    int fields;

    if (!dir || gated)
        return;
    snprintf(arm, sizeof(arm), "%s/arm", dir);
    file = fopen(arm, "r");
    if (!file)
        return;
    fields = fscanf(file, "%15s %31s", want_phase, key);
    fclose(file);
    if (fields != 2 || strcmp(want_phase, phase) != 0)
        return;
    if (path)
    {
        const char *base = strrchr(path, '/');
        char tids[48];

        base = base ? base + 1 : path;
        snprintf(tids, sizeof(tids), "%s.tids", key);
        if (strncmp(base, tids, strlen(tids)) != 0)
            return;
    }

    gated = 1;
    marker(dir, "entered");
    for (int i = 0; i < 2000; ++i)
    {
        struct timespec slice = {.tv_nsec = 10000000};

        if (access(arm, F_OK) != 0)
            return;
        nanosleep(&slice, NULL);
    }
    _exit(92);
}

int
usleep(useconds_t usec)
{
    int (*next)(useconds_t) = dlsym(RTLD_NEXT, "usleep");

    /* Pause only the CPU shim's configured delay, outside the mutex for
     * ordinary single-corpus BUILD. */
    if (usec == 137000)
        pause_build("gpu", NULL);
    return next(usec);
}

int
fsync(int fd)
{
    int (*next)(int) = dlsym(RTLD_NEXT, "fsync");
    int rc = next(fd);

    if (rc == 0 && !gated)
    {
        char link[64], path[PATH_MAX];
        ssize_t bytes;

        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        bytes = readlink(link, path, sizeof(path) - 1);
        if (bytes >= 4)
        {
            path[bytes] = '\0';
            if (strcmp(path + bytes - 4, ".tmp") == 0)
                pause_build("staged", path);
        }
    }
    return rc;
}

int
rename(const char *old, const char *new)
{
    int (*next)(const char *, const char *) = dlsym(RTLD_NEXT, "rename");
    int rc = next(old, new);

    if (rc == 0)
        pause_build("published", new);
    return rc;
}

int
close(int fd)
{
    int (*next)(int) = dlsym(RTLD_NEXT, "close");
    struct sockaddr_un addr = {0};
    socklen_t len = sizeof(addr);
    const char *socket_path = getenv("PGCUVS_BUILD_GATE_SOCKET");
    int done = gated && socket_path &&
        getsockname(fd, (struct sockaddr *) &addr, &len) == 0 &&
        addr.sun_family == AF_UNIX && strcmp(socket_path, addr.sun_path) == 0;
    int rc = next(fd);

    if (done)
    {
        gated = 0;
        marker(getenv("PGCUVS_BUILD_GATE_DIR"), "done");
    }
    return rc;
}

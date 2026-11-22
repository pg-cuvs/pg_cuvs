/*
 * #224: a connect() that waits for room in a full daemon listen backlog must
 * keep waiting, in slices, within the call's budget: after a signal (EINTR)
 * and after an expired slice (EAGAIN). It must end as CANCELED when a query
 * cancel is pending, also when the cancel's signal came before connect() and
 * left no EINTR, and the connection must get the full send timeout back.
 *
 * The real cuvs_ipc.c client against a listener whose backlog is filled and
 * not accepted. The handler is installed with SA_RESTART, as PostgreSQL
 * installs its handlers: the send timeout on the client socket is what makes
 * the kernel return EINTR anyway (signal(7)). Linux-only: on macOS a full
 * backlog refuses at once (ECONNREFUSED) instead of waiting.
 */
#define _GNU_SOURCE 1

#include "cuvs_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int g_failures;
#define CHECK(label, cond)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s (line %d)\n", (label), __LINE__);      \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define MAX_FILL 16

static volatile sig_atomic_t g_signals;
static volatile sig_atomic_t g_cancel_on_signal;
static volatile sig_atomic_t g_cancel_pending;

/* Stands in for PostgreSQL's handlers: count, and on request raise the flag
 * the wait callback reports (QueryCancelPending in the backend). */
static void
on_signal(int signo)
{
    (void)signo;
    g_signals++;
    if (g_cancel_on_signal)
        g_cancel_pending = 1;
}

static int
wait_cb(void)
{
    return g_cancel_pending ? 1 : 0;
}

typedef enum {
    SERVE,        /* signals while waiting, then room: success */
    CANCEL,       /* one signal that brings a cancel */
    STARVE,       /* signals faster than the budget, never room */
    EARLY_CANCEL, /* cancel already pending, no signal at all */
    LATE_ROOM     /* no signal, room only after several slices */
} Mode;

typedef struct Peer {
    const char *path;
    Mode        mode;
    int         listener;
    int         fill[MAX_FILL];
    int         n_fill;
    pthread_t   client;
    pid_t       client_tid;
    volatile int client_done;
    int         saw_connect;   /* /proc showed the client in connect() (diagnostic) */
    uint32_t    reply_status;
    int         served;
    int         n_client_fds;  /* client sockets found while serving */
    long long   client_snd_us; /* their SO_SNDTIMEO / SO_RCVTIMEO */
    long long   client_rcv_us;
} Peer;

static double
now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void
sleep_ms(int ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        ;
}

/* 1 once /proc shows the client thread blocked in connect(2), 0 after 300 ms.
 * Diagnostic only: under qemu user emulation the file shows the host's
 * syscall number, and 300 ms is ample for the client to reach connect(). The
 * mutations (test_ipc_connect_eintr_mutations.py) prove the scenarios reach
 * the wait: without the retry every one of them fails. */
static int
wait_client_in_connect(pid_t tid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", (int)tid);
    for (int i = 0; i < 60; i++)
    {
        FILE *f = fopen(path, "r");
        long nr = -1;
        if (f)
        {
            if (fscanf(f, "%ld", &nr) != 1)
                nr = -1;
            fclose(f);
        }
        if (nr == SYS_connect)
            return 1;
        sleep_ms(5);
    }
    return 0;
}

/* Listener with backlog 0, then nonblocking connects until the kernel says
 * the queue is full: the next blocking connect() has to wait. */
static int
open_full_listener(Peer *p)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", p->path);
    unlink(p->path);
    p->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (p->listener < 0 || bind(p->listener, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(p->listener, 0) != 0)
        return -1;
    p->n_fill = 0;
    while (p->n_fill < MAX_FILL)
    {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0)
            return -1;
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
        {
            p->fill[p->n_fill++] = fd;
            continue;
        }
        close(fd);
        return errno == EAGAIN ? 0 : -1;   /* EAGAIN: the backlog is full */
    }
    return -1;
}

static long long
timeo_us(int fd, int opt)
{
    struct timeval tv;
    socklen_t len = sizeof(tv);
    if (getsockopt(fd, SOL_SOCKET, opt, &tv, &len) != 0)
        return -1;
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* The client socket lives in this process: it is the one fd, other than the
 * fillers, whose peer is the listener's path. Read the timeouts the product
 * code left on it. */
static void
inspect_client_socket(Peer *p, int accepted)
{
    for (int fd = 3; fd < 1024; fd++)
    {
        struct sockaddr_un peer;
        socklen_t len = sizeof(peer);
        int is_fill = 0;

        if (fd == p->listener || fd == accepted)
            continue;
        for (int i = 0; i < p->n_fill; i++)
            is_fill |= (fd == p->fill[i]);
        if (is_fill)
            continue;
        memset(&peer, 0, sizeof(peer));
        if (getpeername(fd, (struct sockaddr *)&peer, &len) != 0 ||
            peer.sun_family != AF_UNIX || strcmp(peer.sun_path, p->path) != 0)
            continue;
        p->n_client_fds++;
        p->client_snd_us = timeo_us(fd, SO_SNDTIMEO);
        p->client_rcv_us = timeo_us(fd, SO_RCVTIMEO);
    }
}

/* Accept the queued fillers (that frees the slot the client waits for), then
 * answer one request with a header of reply_status and no results. */
static void
serve_client(Peer *p)
{
    for (int i = 0; i < p->n_fill; i++)
    {
        int c = accept(p->listener, NULL, NULL);
        if (c >= 0)
            close(c);
    }
    struct pollfd pfd = {.fd = p->listener, .events = POLLIN};
    if (poll(&pfd, 1, 5000) != 1)
        return;   /* the client gave up: nothing left to serve */
    int c = accept(p->listener, NULL, NULL);
    if (c < 0)
        return;
    CuvsCmdFrame cmd;
    if (recv(c, &cmd, sizeof(cmd), MSG_WAITALL) == (ssize_t)sizeof(cmd))
    {
        CuvsReplyHeader r;
        inspect_client_socket(p, c);   /* the client now waits for the reply */
        memset(&r, 0, sizeof(r));
        r.status = p->reply_status;
        r.n_results = 0;
        if (write(c, &r, sizeof(r)) == (ssize_t)sizeof(r))
            p->served = 1;
    }
    close(c);
}

static void
wait_client_done(Peer *p, int ms)
{
    for (int i = 0; i < ms / 10 && !p->client_done; i++)
        sleep_ms(10);
}

static void *
peer_main(void *arg)
{
    Peer *p = (Peer *)arg;

    p->saw_connect = wait_client_in_connect(p->client_tid);
    switch (p->mode)
    {
        case SERVE:
            /* Three signals while the client waits, then make room. */
            for (int i = 0; i < 3 && !p->client_done; i++)
            {
                pthread_kill(p->client, SIGUSR1);
                sleep_ms(50);
            }
            serve_client(p);
            break;
        case CANCEL:
            pthread_kill(p->client, SIGUSR1);
            wait_client_done(p, 5000);
            break;
        case STARVE:
            /* Signals faster than the budget, never any room: only the
             * budget can end the call. Stop after 5 s in any case. */
            for (int i = 0; i < 100 && !p->client_done; i++)
            {
                pthread_kill(p->client, SIGUSR1);
                sleep_ms(50);
            }
            break;
        case EARLY_CANCEL:
            /* No signal: only the check after an expired slice can see the
             * cancel. If the client is still waiting after 3 s, let it in,
             * so a client that misses the cancel ends as OK, not hung. */
            wait_client_done(p, 3000);
            if (!p->client_done)
                serve_client(p);
            break;
        case LATE_ROOM:
            /* No signal; room after 600 ms, i.e. after two expired slices. */
            sleep_ms(600);
            if (!p->client_done)
                serve_client(p);
            break;
    }
    return NULL;
}

static void
close_peer(Peer *p)
{
    for (int i = 0; i < p->n_fill; i++)
        close(p->fill[i]);
    if (p->listener >= 0)
        close(p->listener);
    unlink(p->path);
}

/* Run one scenario: the calling thread is the client. */
static int
run(const char *path, Mode mode, int use_search, double *elapsed, Peer *out)
{
    Peer p;
    pthread_t t;
    int rc;

    memset(&p, 0, sizeof(p));
    p.path = path;
    p.mode = mode;
    p.listener = -1;
    p.reply_status = use_search ? CUVS_STATUS_OK : CUVS_STATUS_NOT_FOUND;
    g_signals = 0;
    g_cancel_on_signal = (mode == CANCEL);
    g_cancel_pending = (mode == EARLY_CANCEL);
    if (open_full_listener(&p) != 0 || p.n_fill < 1)
    {
        close_peer(&p);
        return -100;
    }
    p.client = pthread_self();
    p.client_tid = (pid_t)syscall(SYS_gettid);
    pthread_create(&t, NULL, peer_main, &p);

    double t0 = now_s();
    if (use_search)
    {
        float q[4] = {1, 0, 0, 0};
        uint64_t tids[4];
        float dist[4];
        int n = -1;
        rc = cuvs_ipc_search(path, 1, 2, q, 4, 4, CUVS_METRIC_L2, 0, 0, 0, 0, 0, 0, 0, 0,
                             tids, dist, &n, NULL, NULL);
    }
    else
    {
        CuvsServerSettings s;
        rc = cuvs_ipc_server_settings(path, &s);   /* 1 s connect budget */
    }
    *elapsed = now_s() - t0;
    p.client_done = 1;
    pthread_join(t, NULL);
    close_peer(&p);
    *out = p;
    return rc;
}

int
main(void)
{
    char path[96];
    struct sigaction sa;
    double el;
    Peer p;
    int rc;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    cuvs_ipc_set_wait_callback(wait_cb);
    snprintf(path, sizeof(path), "/tmp/pgcuvs-eintr-%d.sock", (int)getpid());

    /* A: interrupted while the backlog is full, then served -> OK, and the
     * connection carries the full 30 s timeouts, not the last slice. */
    rc = run(path, SERVE, 1, &el, &p);
    CHECK("A setup", rc != -100);
    CHECK("A signals delivered", g_signals >= 1);
    CHECK("A search succeeds after EINTR", rc == CUVS_STATUS_OK);
    CHECK("A request reached the daemon", p.served);
    CHECK("A one client socket found", p.n_client_fds == 1);
    CHECK("A SO_SNDTIMEO restored to 30 s", p.client_snd_us == 30000000LL);
    CHECK("A SO_RCVTIMEO is 30 s", p.client_rcv_us == 30000000LL);
    fprintf(stderr, "A: rc=%d signals=%d served=%d in_connect=%d snd_us=%lld rcv_us=%lld "
            "elapsed=%.3fs\n", rc, (int)g_signals, p.served, p.saw_connect,
            p.client_snd_us, p.client_rcv_us, el);

    /* B: the signal carries a query cancel -> CANCELED, at once. */
    rc = run(path, CANCEL, 1, &el, &p);
    CHECK("B setup", rc != -100);
    CHECK("B canceled, not unavailable", rc == CUVS_STATUS_CANCELED);
    CHECK("B returns promptly", el < 3.0);
    fprintf(stderr, "B: rc=%d signals=%d in_connect=%d elapsed=%.3fs\n",
            rc, (int)g_signals, p.saw_connect, el);

    /* C: signals keep coming and no slot ever frees -> the 1 s budget ends
     * the retries; neither the first EINTR nor endless retrying. */
    rc = run(path, STARVE, 0, &el, &p);
    CHECK("C setup", rc != -100);
    CHECK("C unavailable", rc == CUVS_STATUS_UNAVAILABLE);
    CHECK("C retried until the budget", el >= 0.9);
    CHECK("C bounded by the budget", el < 2.5);
    CHECK("C several signals", g_signals >= 5);
    fprintf(stderr, "C: rc=%d signals=%d in_connect=%d elapsed=%.3fs\n",
            rc, (int)g_signals, p.saw_connect, el);

    /* D: the cancel is already pending and no signal comes (its signal was
     * handled before connect() started) -> CANCELED within about one slice. */
    rc = run(path, EARLY_CANCEL, 1, &el, &p);
    CHECK("D setup", rc != -100);
    CHECK("D no signal", g_signals == 0);
    CHECK("D canceled without EINTR", rc == CUVS_STATUS_CANCELED);
    CHECK("D within about one slice", el < 1.0);
    fprintf(stderr, "D: rc=%d signals=%d in_connect=%d elapsed=%.3fs\n",
            rc, (int)g_signals, p.saw_connect, el);

    /* E: no signal; room comes after two expired slices -> the call keeps
     * waiting (EAGAIN within budget is a retry) and reaches the daemon. */
    rc = run(path, LATE_ROOM, 0, &el, &p);
    CHECK("E setup", rc != -100);
    CHECK("E no signal", g_signals == 0);
    CHECK("E reached the daemon after expired slices", rc == CUVS_STATUS_NOT_FOUND);
    CHECK("E served", p.served);
    CHECK("E waited for the room", el >= 0.5);
    fprintf(stderr, "E: rc=%d signals=%d in_connect=%d elapsed=%.3fs\n",
            rc, (int)g_signals, p.saw_connect, el);

    if (g_failures)
    {
        fprintf(stderr, "test_ipc_connect_eintr: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ipc_connect_eintr: all checks passed\n");
    return 0;
}

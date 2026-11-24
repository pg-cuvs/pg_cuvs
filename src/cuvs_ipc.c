/*
 * cuvs_ipc.c — UDS client for pg_cuvs_server communication.
 *
 * Protocol: send CuvsCmdFrame over UDS, vector payload via shm_open,
 * receive CuvsReplyHeader + n×CuvsResult over UDS.
 *
 * Called from PostgreSQL backend process context. All paths that fail
 * to reach the daemon return an error code without crashing the backend;
 * pg_cuvs.c decides per call site whether that becomes a CPU fallback, a
 * WARNING or an ERROR (an already planned scan raises an ERROR).
 */

/* This file is compiled twice: once via PGXS into the extension .so (PG
 * backend context) and once standalone into pg_cuvs_server (Makefile's
 * SERVER_CFLAGS adds -D_POSIX_C_SOURCE=200809L, which hides glibc's
 * __USE_MISC extensions — including getrandom() and, on this glibc,
 * `struct ucred`/SO_PEERCRED (#87 finding 1) — used below for shm key
 * hardening. __USE_MISC alone (_DEFAULT_SOURCE) wasn't enough to expose
 * struct ucred in CI; _GNU_SOURCE is the strict superset that reliably does.
 * Re-enable them; additive alongside _POSIX_C_SOURCE. Must precede any
 * system header. Mirrors the guard in pg_cuvs_server.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "cuvs_ipc.h"
#include "cuvs_util.h"   /* leveled logging macros (LOG_ERROR/WARN/INFO/DEBUG) */
#include "cuvs_build_corpus.h"   /* ADR-057: tiered corpus handoff + SCM_RIGHTS */
#include "cuvs_hnsw_build_common.h"

/* Hot-path IPC traces (LOG_DEBUG) are gated by PG_CUVS_DEBUG; enable via
 * -DPG_CUVS_DEBUG=1. Error paths (LOG_ERROR) always log unconditionally. */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/random.h>   /* getrandom(2): CSPRNG suffix for shm key names, #87 */
#include <fcntl.h>
#include <time.h>
#include <poll.h>   /* 3S: interruptible reply wait */

/* Circuit breaker state machine moved to cuvs_util.c (structural commit). */

/* ----------------------------------------------------------------
 * Socket helpers
 * ---------------------------------------------------------------- */

/* #119: expected owner uid of cuvs.socket_path (-1 = no check). Set via
 * cuvs_ipc_set_daemon_uid, mirroring the g_wait_cb setter pattern below. */
static int g_expected_daemon_uid = -1;
static uint32_t g_cagra_itopk_size = 0;
static uint32_t g_cagra_search_width = 0;
/* 3S: query-cancel / statement_timeout hook. The backend registers a callback
 * (cuvs_ipc_set_wait_callback) that reports whether an interrupt is pending;
 * the search reply-wait and every connect() wait (#224) poll it. */
static int (*g_wait_cb)(void) = NULL;

void
cuvs_ipc_set_cagra_search_params(uint32_t itopk_size, uint32_t search_width)
{
    g_cagra_itopk_size = itopk_size;
    g_cagra_search_width = search_width;
}

void
cuvs_ipc_set_daemon_uid(int uid)
{
    g_expected_daemon_uid = uid;
}

/* Slice of one connect() wait (#224), the same 250 ms as the reply wait in
 * recv_all_interruptible: between slices the wait callback is asked again. */
#define CUVS_CONNECT_SLICE_US 250000

/* Open a UDS connection. Returns fd on success, -1 on failure with errno set
 * to: the errno of the call that failed (socket, lstat, connect, getsockopt);
 * ENOTSOCK if socket_path is not a socket; EACCES if the socket's owner or the
 * peer's uid is not the expected daemon uid (cuvs.daemon_uid); ECANCELED if
 * the wait callback (3S) reported a pending cancel while connect() waited.
 * recv_timeout_sec: SO_RCVTIMEO/SO_SNDTIMEO of the connection (longer for
 *   requests whose reply is slow, e.g. builds), and the upper bound of the
 *   whole connect() wait including its retries (#224). All callers pass > 0.
 * canceled: if non-NULL, set to 1 for the ECANCELED case, else 0. */
static void forget_reply(void);

static int
uds_connect_wait(const char *socket_path, int recv_timeout_sec, int *canceled)
{
    int fd;

    if (canceled)
        *canceled = 0;
    forget_reply();   /* every request starts without a previous reply's I3 fields */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        int err = errno;
        LOG_WARN("[cuvs_ipc] socket() for %s failed: %s (errno=%d)\n",
                 socket_path, strerror(err), err);
        errno = err;
        return -1;
    }

    /* #119: verify socket_path owner before connect(). This is a fast-path
     * pre-check only — the actual security boundary is the post-connect
     * SO_PEERCRED check below (F1: this lstat is TOCTOU-racy on its own; a
     * kernel-verified peer credential after connect() is not). Checking
     * here first still buys two things: a cheaper failure (no connect()
     * syscall for an obviously-wrong path) and a clearer error message
     * (owner mismatch vs. generic peer-uid mismatch). lstat (not stat) so a
     * symlink swap to a different socket can't bypass this pre-check —
     * though connect() itself follows the symlink regardless, so the
     * post-connect check below is what actually catches that case. */
    if (g_expected_daemon_uid >= 0 && socket_path[0] != '\0')
    {
        struct stat st;
        if (lstat(socket_path, &st) != 0)
        {
            int err = errno;
            /* ENOENT is the normal "daemon is down" shape (restart window,
             * crash, not yet started) — happens on every query while the
             * daemon is out; don't drown a real hijack signal in restart
             * noise. Anything else (EACCES, ELOOP, ...) is unusual enough
             * to warrant the louder level. */
            if (err == ENOENT)
                LOG_WARN("[cuvs_ipc] cannot stat socket_path %s: %s\n",
                          socket_path, strerror(err));
            else
                LOG_ERROR("[cuvs_ipc] cannot stat socket_path %s: %s\n",
                          socket_path, strerror(err));
            close(fd);
            errno = err;
            return -1;
        }
        if (!S_ISSOCK(st.st_mode))
        {
            LOG_ERROR("[cuvs_ipc] socket_path %s is not a socket\n", socket_path);
            close(fd);
            errno = ENOTSOCK;
            return -1;
        }
        if (st.st_uid != (uid_t) g_expected_daemon_uid)
        {
            LOG_ERROR("[cuvs_ipc] socket_path %s owned by uid=%u, expected=%d "
                      "— refusing to connect (possible hijack)\n",
                      socket_path, (unsigned) st.st_uid, g_expected_daemon_uid);
            close(fd);
            errno = EACCES;
            return -1;
        }
        /* No S_IWOTH check: connect() needs write access to the socket
         * inode (AF_UNIX requirement), but that's not the same as
         * world-write. The daemon itself creates the socket 0660
         * (pg_cuvs_server.c) — the correct different-uid deployment adds
         * the backend's OS user to the daemon's group, not a wider mode.
         * (The Brev dev bootstrap widens it to 0666 as a convenience
         * because it doesn't set up that group membership; don't carry
         * that into production.) Either way, S_IWOTH carries no attacker
         * advantage the uid check above doesn't already close: a
         * resquatted socket is owned by the attacker's uid, not
         * g_expected_daemon_uid, so it's refused above regardless of its
         * mode. */
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    struct timeval tv = {.tv_sec = recv_timeout_sec, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* #224: when the daemon's listen backlog is full, a blocking connect()
     * waits for a slot, bounded by SO_SNDTIMEO (EAGAIN when it expires). A
     * socket call with a send timeout is never restarted after a signal
     * handler, SA_RESTART or not (signal(7)): the kernel returns EINTR instead
     * of ERESTARTSYS (sock_intr_errno() in include/net/sock.h). PostgreSQL 15
     * and later arm IDLE_STATS_UPDATE_TIMEOUT (PGSTAT_IDLE_INTERVAL, 10 s)
     * when a backend goes idle with stats left to flush and do not disarm it
     * when the next query starts (src/backend/tcop/postgres.c: "we don't
     * disable the timeout below"), so a busy backend can receive SIGALRM while
     * it runs queries; its handlers are SA_RESTART (src/port/pqsignal.c). A busy
     * but live daemon therefore used to look unreachable.
     *
     * Calling connect() again on the same fd is safe for AF_UNIX stream
     * sockets on Linux: unix_stream_connect() (net/unix/af_unix.c, checked in
     * v5.15 and master) waits for the backlog before it touches the client
     * socket's state, and on a signal or a timeout it frees the half-built
     * peer and returns with the socket still TCP_CLOSE. Nothing is left in
     * progress, unlike TCP, where a second connect() would see EALREADY.
     *
     * The wait runs in slices of CUVS_CONNECT_SLICE_US (SO_SNDTIMEO), all
     * within one monotonic budget of recv_timeout_sec. After every failed
     * attempt the wait callback is asked first: a cancel whose signal arrived
     * before connect() started leaves no EINTR, and the slice bounds how long
     * it goes unseen. EINTR and EAGAIN (a slice expired) retry while budget is
     * left; anything else fails at once. */
    {
        struct timespec start;
        int attempts = 0;
        int connected = 0;
        int err = 0;

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;)
        {
            struct timespec now;
            struct timeval slice;
            long long left_us;
            long long slice_us;

            clock_gettime(CLOCK_MONOTONIC, &now);
            left_us = (long long) recv_timeout_sec * 1000000
                    - ((long long) (now.tv_sec - start.tv_sec) * 1000000
                       + (now.tv_nsec - start.tv_nsec) / 1000);
            if (attempts > 0 && left_us <= 0)
                break;              /* budget spent: err is the last attempt's */
            slice_us = left_us < CUVS_CONNECT_SLICE_US ? left_us : CUVS_CONNECT_SLICE_US;
            if (slice_us < 1)
                slice_us = 1;       /* a zero timeval would mean no timeout */
            slice.tv_sec = (time_t) (slice_us / 1000000);
            slice.tv_usec = (suseconds_t) (slice_us % 1000000);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &slice, sizeof(slice));
            attempts++;
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            {
                connected = 1;
                break;
            }
            err = errno;
            if (g_wait_cb && g_wait_cb())
            {
                if (canceled)
                    *canceled = 1;
                close(fd);
                errno = ECANCELED;
                return -1;
            }
            if (err != EINTR && err != EAGAIN)
                break;              /* not a wait for the backlog */
        }
        if (!connected)
        {
            /* ENOENT and ECONNREFUSED are the "daemon is down" shapes (no
             * socket file, or a stale one without a listener). They repeat on
             * every query while the daemon is out and the caller already
             * reports that, so they stay at DEBUG. Anything else (EAGAIN: the
             * backlog stayed full for the whole budget, EINTR: the budget ran
             * out on a signal, EACCES, ...) means a daemon that is there but
             * could not be reached, which an operator needs to see. */
            if (err == ENOENT || err == ECONNREFUSED)
                LOG_DEBUG("[cuvs_ipc] connect to %s failed: %s (errno=%d)\n",
                          socket_path, strerror(err), err);
            else
                LOG_WARN("[cuvs_ipc] connect to %s failed: %s (errno=%d, "
                         "%d attempts, budget %d s)\n",
                         socket_path, strerror(err), err, attempts, recv_timeout_sec);
            close(fd);
            errno = err;
            return -1;
        }
        /* The request itself gets the full send timeout, not the last slice. */
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    /* #119 (adversarial review F1): post-connect SO_PEERCRED check. The
     * lstat above is a TOCTOU race — the daemon could be unlinked and
     * resquatted between the lstat and this connect(). SO_PEERCRED is
     * kernel-verified at connect() time and race-free: the peer credential
     * is latched to whoever accepted this specific connection, and can't be
     * swapped out afterward. This is the actual security boundary; the
     * lstat check above stays only for a fast failure and a clearer error
     * message before paying for a connect(). Linux-only, like the shm-side
     * SO_PEERCRED check (shm_check_daemon_owner) this mirrors. */
#ifdef __linux__
    if (g_expected_daemon_uid >= 0)
    {
        struct ucred cred;
        socklen_t    cred_len = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0)
        {
            int err = errno;
            LOG_ERROR("[cuvs_ipc] SO_PEERCRED failed: %s — refusing to connect\n",
                      strerror(err));
            close(fd);
            errno = err;
            return -1;
        }
        if (cred.uid != (uid_t) g_expected_daemon_uid)
        {
            LOG_ERROR("[cuvs_ipc] socket peer uid=%u, expected=%d "
                      "— refusing to connect (possible hijack)\n",
                      (unsigned) cred.uid, g_expected_daemon_uid);
            close(fd);
            errno = EACCES;
            return -1;
        }
    }
#endif

    return fd;
}

static int
uds_connect_ex(const char *socket_path, int recv_timeout_sec)
{
    return uds_connect_wait(socket_path, recv_timeout_sec, NULL);
}

static int
uds_connect(const char *socket_path)
{
    return uds_connect_ex(socket_path, 30); /* default 30s for SEARCH */
}

static int
send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;   /* retry: a signal interrupted the write */
            return -1;
        }
        if (n == 0)
            return -1;      /* peer closed */
        sent += (size_t)n;
    }
    return 0;
}

/* Every client-side reply header is read through these two, so per-reply
 * bookkeeping has a single home. Same return contract as recv_all and
 * recv_all_interruptible. */
static int recv_all(int fd, void *buf, size_t len);
static int recv_all_interruptible(int fd, void *buf, size_t len, int timeout_sec);

static __thread char g_last_err_out[CUVS_ADMISSION_ERR_OUT_BYTES];
static __thread char g_last_notice_out[CUVS_NOTICE_OUT_BYTES];

const char *
cuvs_ipc_last_err_out(void)
{
    return g_last_err_out;
}

const char *
cuvs_ipc_last_notice_out(void)
{
    return g_last_notice_out;
}

static void
forget_reply(void)
{
    g_last_err_out[0] = '\0';
    g_last_notice_out[0] = '\0';
}

/* Copy the I3 fields; a daemon that fills a field to the brim without a NUL
 * is cut at the field size, never read past it. */
static void
remember_reply(const CuvsReplyHeader *hdr)
{
    memcpy(g_last_err_out, hdr->err_out, sizeof(g_last_err_out));
    g_last_err_out[sizeof(g_last_err_out) - 1] = '\0';
    memcpy(g_last_notice_out, hdr->notice_out, sizeof(g_last_notice_out));
    g_last_notice_out[sizeof(g_last_notice_out) - 1] = '\0';
}

static int
recv_reply(int fd, CuvsReplyHeader *hdr)
{
    int rc = recv_all(fd, hdr, sizeof(*hdr));
    if (rc == 0)
        remember_reply(hdr);
    return rc;
}

static int
recv_reply_interruptible(int fd, CuvsReplyHeader *hdr, int timeout_sec)
{
    int rc = recv_all_interruptible(fd, hdr, sizeof(*hdr), timeout_sec);
    if (rc == 0)
        remember_reply(hdr);
    return rc;
}

/*
 * #77: stamp the protocol identity, then send the command frame. Every
 * CuvsCmdFrame leaves the backend through here, so a newly added op cannot
 * forget the magic/version the daemon validates.
 */
static int
send_cmd(int fd, CuvsCmdFrame *cmd)
{
    cmd->proto_magic = CUVS_PROTO_MAGIC;
    cmd->proto_version = CUVS_PROTO_VERSION;
    if (cmd->op == CUVS_OP_SEARCH || cmd->op == CUVS_OP_SEARCH_BATCH)
    {
        cmd->itopk_size = g_cagra_itopk_size;
        cmd->search_width = g_cagra_search_width;
    }
    return send_all(fd, cmd, sizeof(*cmd));
}

static int
recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t received = 0;
    while (received < len)
    {
        ssize_t n = read(fd, p + received, len - received);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;   /* retry: a signal interrupted the read. Large
                             * builds block here for minutes, so a stray
                             * backend signal must not abort the wait and
                             * spuriously fail a CREATE INDEX the daemon
                             * actually completed. */
            return -1;
        }
        if (n == 0)
            return -1;      /* peer closed */
        received += (size_t)n;
    }
    return 0;
}

/* 3S: the backend registers g_wait_cb (defined with the socket helpers). */
void
cuvs_ipc_set_wait_callback(int (*cb)(void))
{
    g_wait_cb = cb;
}

/* Interruptible recv: like recv_all, but poll()s in short slices so a pending
 * PG cancel/statement_timeout (reported by g_wait_cb) can abort a long reply
 * wait instead of blocking the backend uninterruptibly. Returns 0 on full read,
 * -1 on error/EOF/overall-timeout, 1 if the wait callback asked to abort. */
static int
recv_all_interruptible(int fd, void *buf, size_t len, int timeout_sec)
{
    char  *p = buf;
    size_t got = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (got < len)
    {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 250 /* ms slice */);

        if (pr < 0)
        {
            if (errno == EINTR)
            {
                if (g_wait_cb && g_wait_cb())
                    return 1;   /* signal delivered a pending cancel */
                continue;
            }
            return -1;
        }
        if (pr == 0)            /* slice timed out: check cancel + overall deadline */
        {
            struct timespec now;
            if (g_wait_cb && g_wait_cb())
                return 1;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((now.tv_sec - start.tv_sec) >= timeout_sec)
                return -1;      /* daemon never replied within the budget */
            continue;
        }

        ssize_t n = read(fd, p + got, len - got);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;          /* peer closed */
        got += (size_t)n;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * shm helpers
 * ---------------------------------------------------------------- */

/* CSPRNG fill for shm key suffixes (#87). getrandom(2) is Linux-only; this
 * file also builds via PGXS on whatever host runs `make` (macOS in local
 * dev), so fall back to arc4random_buf() there (BSD/macOS libc, never fails).
 * Returns 0 on success, -1 if the CSPRNG source itself failed (Linux only —
 * arc4random_buf() has no failure mode). #87 finding 2: the caller must fail
 * closed on -1, not degrade to a predictable (all-zero) suffix. */
static int
shm_key_random(unsigned char *buf, size_t len)
{
#ifdef __linux__
    if (getrandom(buf, len, 0) != (ssize_t)len)
        return -1;
#else
    arc4random_buf(buf, len);
#endif
    return 0;
}

/* Build a unique, unguessable shm key from pid + sequence counter + a CSPRNG
 * suffix. The pid/seq prefix stays for debug ordering; the random suffix
 * (#87) is what makes the name unguessable to another user on a shared host.
 * Returns 0 on success, -1 if the CSPRNG source failed — callers must treat
 * that as a hard failure (goto cleanup / CUVS_STATUS_ERROR), not proceed with
 * an unrandomized key (#87 finding 2). */
static int
make_shm_key(char *key, size_t keylen)
{
    static int seq = 0;
    unsigned char rnd[8];
    char hex[17];

    if (shm_key_random(rnd, sizeof(rnd)) != 0)
        return -1;
    for (size_t i = 0; i < sizeof(rnd); i++)
        snprintf(hex + i * 2, 3, "%02x", rnd[i]);

    snprintf(key, keylen, "/pg_cuvs_%d_%d_%s", (int)getpid(), seq++, hex);
    return 0;
}

/* #87 finding 1: verify a reply shm segment was created by the daemon we are
 * actually talking to on `sock`, not by an attacker who unlinked and
 * re-squatted the segment name on a non-sticky /dev/shm host (the name is
 * CSPRNG-random but not secret once an attacker can readdir() /dev/shm).
 * SO_PEERCRED on `sock` gives the kernel-verified uid of the daemon process
 * that accepted this connection; a segment the real daemon created has
 * st_uid == that uid. Linux-only (SO_PEERCRED + struct ucred). */
static int
shm_check_daemon_owner(int sock, int fd)
{
#ifdef __linux__
    struct ucred cred;
    socklen_t    cred_len = sizeof(cred);
    struct stat  st;

    if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0)
        return 0;
    if (fstat(fd, &st) != 0)
        return 0;
    return st.st_uid == cred.uid;
#else
    (void)sock; (void)fd;
    return 1;
#endif
}

/* Write vectors + TIDs into a new shm segment. Returns shm fd or -1. */
static int
shm_write_build_payload(const char   *shm_key,
                        const float  *vecs,
                        const uint64_t *tids,
                        int64_t       n_vecs,
                        int           dim)
{
    size_t vec_bytes = (size_t)n_vecs * dim * sizeof(float);
    size_t tid_bytes = (size_t)n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    int fd = shm_open(shm_key, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0)
        return -1;

    /* umask may be stricter than 0644 (e.g. 077); the daemon runs as a
     * different uid and must retain read access, so guarantee the mode
     * explicitly rather than rely on shm_open's mode argument (#87). */
    fchmod(fd, 0644);

    if (ftruncate(fd, (off_t)total) < 0)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    memcpy(mem, vecs, vec_bytes);
    memcpy((char *)mem + vec_bytes, tids, tid_bytes);

    munmap(mem, total);
    return fd;
}

/* Phase 3M: write [uint32 Q][uint32 dim][Q*dim float32] into a new shm segment
 * for a batch search request. Returns shm fd or -1. */
static int
shm_write_query_batch(const char *shm_key, const float *queries,
                      uint32_t n_queries, int dim)
{
    size_t hdr_bytes = 2 * sizeof(uint32_t);
    size_t vec_bytes = (size_t)n_queries * (size_t)dim * sizeof(float);
    size_t total     = hdr_bytes + vec_bytes;

    int fd = shm_open(shm_key, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0)
        return -1;
    fchmod(fd, 0644);   /* guarantee daemon (other uid) read access; see above (#87) */
    if (ftruncate(fd, (off_t)total) < 0)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }
    ((uint32_t *)mem)[0] = n_queries;
    ((uint32_t *)mem)[1] = (uint32_t)dim;
    memcpy((char *)mem + hdr_bytes, queries, vec_bytes);
    munmap(mem, total);
    return fd;
}

/* Write query vector into a new shm segment. Returns shm fd or -1. */
static int
shm_write_page_bytes(const char *shm_key, const void *query_vec, size_t vec_bytes)
{
    int fd = shm_open(shm_key, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0)
        return -1;

    fchmod(fd, 0644);   /* guarantee daemon (other uid) read access; see above (#87) */

    if (ftruncate(fd, (off_t)vec_bytes) < 0)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    void *mem = mmap(NULL, vec_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    memcpy(mem, query_vec, vec_bytes);
    munmap(mem, vec_bytes);
    return fd;
}

static int
shm_write_query(const char *shm_key, const float *query_vec, int dim)
{
    return shm_write_page_bytes(shm_key, query_vec, (size_t) dim * sizeof(float));
}

static int
shm_write_page_supplement(const char *key, const CuvsPageSupplement *supplement, uint32_t dim)
{
    size_t marks = (size_t) supplement->positions * sizeof(CuvsPagePosition);
    size_t values = (size_t) supplement->vector_rows * dim * sizeof(float);
    size_t bytes = marks + values;
    int fd = shm_open(key, O_CREAT | O_EXCL | O_RDWR, 0644);
    void *memory = MAP_FAILED;

    if (fd < 0)
        return -1;
    if (fchmod(fd, 0644) == 0 && ftruncate(fd, (off_t) bytes) == 0)
        memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED)
    {
        close(fd);
        shm_unlink(key);
        return -1;
    }
    memcpy(memory, supplement->marks, marks);
    if (values)
        memcpy((char *) memory + marks, supplement->vectors, values);
    munmap(memory, bytes);
    return fd;
}

static int
page_overlay_exchange(const char *socket_path, CuvsCmdFrame *cmd, CuvsPageOverlayRequest *frame,
                      const float *queries, const CuvsPageSupplement *supplement,
                      CuvsPageOverlayResult *results, int *count, uint32_t *latency_us,
                      CuvsPageProgress *progress)
{
    CuvsReplyHeader reply;
    int status = CUVS_STATUS_ERROR;
    int query_fd = -1, supplement_fd = -1, sock = -1;
    int canceled, waiting;

    *count = 0;
    *latency_us = 0;
    memset(progress, 0, sizeof(*progress));
    if (cmd->dim == 0 || cmd->dim > 65535 || cmd->n_vecs > 1024 || cmd->k > 4000)
        return status;
    if (queries)
    {
        if (make_shm_key(cmd->shm_key, sizeof(cmd->shm_key)) != 0)
            goto done;
        query_fd = cmd->n_vecs > 1 ?
            shm_write_query_batch(cmd->shm_key, queries, cmd->n_vecs, cmd->dim) :
            shm_write_query(cmd->shm_key, queries, cmd->dim);
        if (query_fd < 0)
            goto done;
    }
    frame->base.length = sizeof(*frame);
    frame->positions = frame->vector_rows = 0;
    if (supplement)
    {
        if (!supplement->positions || supplement->positions > CUVS_PAGE_BATCH_MAX ||
            supplement->vector_rows > supplement->positions || !supplement->marks ||
            (supplement->vector_rows && !supplement->vectors) ||
            make_shm_key(frame->supplement_shm, sizeof(frame->supplement_shm)) != 0)
            goto done;
        supplement_fd = shm_write_page_supplement(frame->supplement_shm, supplement, cmd->dim);
        if (supplement_fd < 0)
            goto done;
        frame->positions = supplement->positions;
        frame->vector_rows = supplement->vector_rows;
    }
    sock = uds_connect_wait(socket_path, 30, &canceled);
    if (sock < 0)
    {
        status = canceled ? CUVS_STATUS_CANCELED : CUVS_STATUS_UNAVAILABLE;
        goto done;
    }
    if (send_cmd(sock, cmd) < 0 || send_all(sock, frame, sizeof(*frame)) < 0)
        goto done;
    waiting = recv_reply_interruptible(sock, &reply, 30);
    if (waiting != 0)
    {
        if (waiting == 1)
            status = CUVS_STATUS_CANCELED;
        goto done;
    }
    status = (int) reply.status;
    if (status == CUVS_STATUS_OK || status == CUVS_STATUS_PAGE_NEED ||
        status == CUVS_STATUS_PAGE_OVERLAY_FULL)
    {
        waiting = recv_all_interruptible(sock, progress, sizeof(*progress), 30);
        if (waiting != 0)
        {
            status = waiting == 1 ? CUVS_STATUS_CANCELED : CUVS_STATUS_ERROR;
            goto done;
        }
        if (status == CUVS_STATUS_OK)
        {
            if (reply.n_results > cmd->n_vecs * cmd->k)
            {
                status = CUVS_STATUS_ERROR;
                goto done;
            }
            waiting = reply.n_results ?
                recv_all_interruptible(sock, results, reply.n_results * sizeof(*results), 30) : 0;
            if (waiting != 0)
                status = waiting == 1 ? CUVS_STATUS_CANCELED : CUVS_STATUS_ERROR;
            else
            {
                *count = (int) reply.n_results;
                *latency_us = reply.latency_us;
            }
        }
    }

done:
    if (sock >= 0)
        close(sock);
    if (query_fd >= 0)
    {
        close(query_fd);
        shm_unlink(cmd->shm_key);
    }
    if (supplement_fd >= 0)
    {
        close(supplement_fd);
        shm_unlink(frame->supplement_shm);
    }
    return status;
}

int
cuvs_ipc_supply_pages(const char *socket_path, uint32_t db_oid, uint32_t index_oid,
    const CuvsPageOverlayRequest *request, int dim, uint32_t metric,
    const CuvsPageSupplement *supplement, CuvsPageProgress *progress)
{
    CuvsCmdFrame cmd = {.op = CUVS_OP_SUPPLY_PAGES, .db_oid = db_oid, .index_oid = index_oid,
        .dim = (uint32_t) dim, .metric = metric};
    CuvsPageOverlayRequest frame = *request;
    int count;
    uint32_t latency_us;

    if (!progress)
        return CUVS_STATUS_ERROR;
    if (!supplement)
    {
        memset(progress, 0, sizeof(*progress));
        return CUVS_STATUS_ERROR;
    }
    return page_overlay_exchange(socket_path, &cmd, &frame, NULL,
                                  supplement, NULL, &count, &latency_us, progress);
}

int
cuvs_ipc_search_page_overlay(const char *socket_path, uint32_t db_oid, uint32_t index_oid,
    const CuvsPageOverlayRequest *request, const float *queries, int nq, int dim, int k,
    uint32_t metric, uint32_t itopk_size, uint32_t search_width, uint32_t use_cpu_hnsw,
    uint32_t search_mode, uint32_t bf_precision, const CuvsPageSupplement *supplement,
    CuvsPageOverlayResult *results, int *count, uint32_t *latency_us, CuvsPageProgress *progress)
{
    CuvsCmdFrame cmd = {.op = nq > 1 ? CUVS_OP_SEARCH_PAGE_OVERLAY_BATCH : CUVS_OP_SEARCH_PAGE_OVERLAY,
        .db_oid = db_oid, .index_oid = index_oid, .dim = (uint32_t) dim, .k = (uint32_t) k,
        .metric = metric, .itopk_size = itopk_size, .search_width = search_width,
        .n_vecs = nq, .search_mode = search_mode, .bf_precision = bf_precision,
        .use_cpu_hnsw = use_cpu_hnsw};
    CuvsPageOverlayRequest frame = *request;

    if (!queries || !results || !count || !latency_us || !progress || nq < 1 || k < 1)
        return CUVS_STATUS_ERROR;
    return page_overlay_exchange(socket_path, &cmd, &frame, queries,
                                  supplement, results, count, latency_us, progress);
}

/* A page reply uses CuvsResult.tid as a zero-based slot number. */
int
cuvs_ipc_search_pages(const char *socket_path, uint32_t db_oid, uint32_t index_oid,
    const CuvsPageSearchRequest *request, const float *queries, int nq, int dim, int k,
    uint32_t metric, uint32_t itopk_size, uint32_t search_width, uint32_t use_cpu_hnsw,
    uint32_t search_mode, uint32_t bf_precision, const uint32_t *excluded, uint32_t words,
    CuvsResult *results, int *count, uint32_t *latency_us)
{
    char key[64] = "", filter_key[64] = "";
    int fd = -1, filter_fd = -1;
    int sock = -1;
    int status = CUVS_STATUS_ERROR;
    int canceled;
    int waiting;
    CuvsReplyHeader reply;
    CuvsCmdFrame cmd = {.op = excluded ? CUVS_OP_SEARCH_PAGES_FILTERED :
            nq > 1 ? CUVS_OP_SEARCH_PAGES_BATCH : CUVS_OP_SEARCH_PAGES,
        .db_oid = db_oid,
        .index_oid = index_oid, .dim = (uint32_t) dim, .k = (uint32_t) k,
        .metric = metric, .itopk_size = itopk_size, .search_width = search_width,
        .n_vecs = nq, .n_filter_tids = words, .search_mode = search_mode,
        .bf_precision = bf_precision, .use_cpu_hnsw = use_cpu_hnsw};

    *count = 0;
    *latency_us = 0;
    if (nq < 1 || nq > 1024 || make_shm_key(key, sizeof(key)) != 0)
        goto done;
    fd = nq > 1 ? shm_write_query_batch(key, queries, (uint32_t) nq, dim) :
        shm_write_query(key, queries, dim);
    if (fd < 0)
        goto done;
    if (excluded)
    {
        if (make_shm_key(filter_key, sizeof(filter_key)) != 0)
            goto done;
        filter_fd = shm_write_page_bytes(filter_key, excluded, (size_t) words * sizeof(*excluded));
        if (filter_fd < 0)
            goto done;
        memcpy(cmd.filter_shm_key, filter_key, sizeof(filter_key));
    }
    sock = uds_connect_wait(socket_path, 30, &canceled);
    if (sock < 0)
    {
        status = canceled ? CUVS_STATUS_CANCELED : CUVS_STATUS_UNAVAILABLE;
        goto done;
    }
    memcpy(cmd.shm_key, key, sizeof(key));
    if (send_cmd(sock, &cmd) < 0 || send_all(sock, request, sizeof(*request)) < 0)
        goto done;

    waiting = recv_reply_interruptible(sock, &reply, 30);
    if (waiting != 0)
    {
        if (waiting == 1)
            status = CUVS_STATUS_CANCELED;
        goto done;
    }
    status = (int) reply.status;
    if (status == CUVS_STATUS_OK)
    {
        if (reply.n_results > (uint32_t) (nq * k) ||
            (reply.n_results != 0 &&
             recv_all(sock, results, reply.n_results * sizeof(*results)) < 0))
            status = CUVS_STATUS_ERROR;
        else
        {
            *count = (int) reply.n_results;
            *latency_us = reply.latency_us;
        }
    }

done:
    if (sock >= 0)
        close(sock);
    if (fd >= 0)
    {
        close(fd);
        shm_unlink(key);
    }
    if (filter_fd >= 0)
    {
        close(filter_fd);
        shm_unlink(filter_key);
    }
    return status;
}

void
cuvs_ipc_mark_pages_empty(const char *socket_path, uint32_t db_oid, uint32_t index_oid,
                          const char *index_dir, const CuvsPageBuildRequest *request)
{
    CuvsCmdFrame cmd = {.op = CUVS_OP_MARK_PAGES_EMPTY, .db_oid = db_oid, .index_oid = index_oid};
    CuvsReplyHeader reply;
    char dir[256] = "";
    int sock;

    if (!socket_path || !socket_path[0])
        return;
    sock = uds_connect_ex(socket_path, 1);
    if (sock < 0)
        return;
    if (index_dir)
        strncpy(dir, index_dir, sizeof(dir) - 1);
    if (send_cmd(sock, &cmd) == 0 && send_all(sock, request, sizeof(*request)) == 0 &&
        cuvs_fd_send(sock, -1, dir, sizeof(dir)) == 0)
        (void) recv_all(sock, &reply, sizeof(reply));
    close(sock);
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *query_vec,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      shard_overfetch,
    int           parallel_fanout,
    uint32_t      use_cpu_hnsw,
    uint32_t      search_mode,
    uint32_t      bf_precision,
    uint32_t      bf_batch_wait_us,
    uint32_t      cagra_batch_wait_us,
    uint32_t      delta_search_mode,
    uint64_t     *tids_out,
    float        *dist_out,
    int          *n_out,
    uint32_t     *latency_us_out,
    int          *delta_merged_out)
{
    char shm_key[64] = "";   /* stays empty if make_shm_key fails */
    int  shm_fd = -1;
    int  sock   = -1;
    int  rc     = CUVS_STATUS_ERROR;

    if (latency_us_out)   *latency_us_out = 0;
    if (n_out)            *n_out = 0;
    if (delta_merged_out) *delta_merged_out = 0;

    if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
        goto cleanup;

    shm_fd = shm_write_query(shm_key, query_vec, dim);
    if (shm_fd < 0)
        goto cleanup;       /* rc stays CUVS_STATUS_ERROR — our side issue */

    {
        int canceled;
        sock = uds_connect_wait(socket_path, 30 /* sec budget */, &canceled);
        if (sock < 0) {
            /* #224: a cancel that interrupted the connect is not a daemon
             * failure; report it like a canceled reply wait (3S). */
            rc = canceled ? CUVS_STATUS_CANCELED
                          : CUVS_STATUS_UNAVAILABLE;   /* daemon not reachable */
            goto cleanup;
        }
    }

    CuvsCmdFrame cmd = {
        .op              = CUVS_OP_SEARCH,
        .db_oid          = db_oid,
        .index_oid       = index_oid,
        .k               = (uint32_t)k,
        .metric          = metric,
        .dim             = (uint32_t)dim,
        .n_vecs          = 0,
        .shard_overfetch = shard_overfetch,
        .parallel_fanout = (uint32_t)(parallel_fanout ? 1 : 0),
        .use_cpu_hnsw    = use_cpu_hnsw,
        .search_mode     = search_mode,
        .bf_precision    = bf_precision,
        .bf_batch_wait_us = bf_batch_wait_us,
        .cagra_batch_wait_us = cagra_batch_wait_us,
        .delta_search_mode = delta_search_mode,
    };
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* 3S: wait for the reply header interruptibly so a query cancel /
     * statement_timeout aborts a long GPU search instead of blocking the backend.
     * On abort we close the socket (cleanup) -> the daemon's reply send fails and
     * its per-connection thread cleans up; the backend raises the interrupt. */
    CuvsReplyHeader hdr;
    {
        int wr = recv_reply_interruptible(sock, &hdr, 30 /* sec budget */);
        if (wr == 1) { rc = CUVS_STATUS_CANCELED; goto cleanup; }
        if (wr < 0)  goto cleanup;
    }

    rc = (int)hdr.status;
    if (latency_us_out)
        *latency_us_out = hdr.latency_us;
    if (delta_merged_out)
        *delta_merged_out = (hdr.delta_merged != 0);

    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n = (int)hdr.n_results;
        /* Defensive: never write more than the caller's k-sized buffers. */
        int n_write = (n > k) ? k : n;
        CuvsResult *results = malloc(n * sizeof(CuvsResult));
        if (!results)
        {
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }

        if (recv_all(sock, results, n * sizeof(CuvsResult)) < 0)
        {
            free(results);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }

        for (int i = 0; i < n_write; i++)
        {
            tids_out[i] = results[i].tid;
            dist_out[i] = results[i].distance;
        }
        *n_out = n_write;
        free(results);
    }
    else
    {
        *n_out = 0;
    }

cleanup:
    if (sock >= 0)
        close(sock);
    if (shm_fd >= 0)
        close(shm_fd);
    if (shm_key[0])
        shm_unlink(shm_key);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search_batch (Phase 3M)
 *
 * Q queries in one request shm ([Q][dim][Q*dim f32]); the daemon runs one
 * batched GPU dispatch and returns Q*K results via a daemon-allocated reply
 * shm ([Q][K][tids Q*K u64][dists Q*K f32]), with the reply shm key in
 * hdr.error[] and K in hdr.delta_merged (reusing the export_adjacency reply
 * mechanism). The backend owns both shm segments and unlinks them here.
 * tids_out/dist_out must hold Q*k; the daemon clamps K = min(k, corpus) and
 * *k_out reports the actual per-query stride.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search_batch(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *queries,
    uint32_t      n_queries,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      shard_overfetch,
    int           parallel_fanout,
    uint32_t      search_mode,
    uint32_t      bf_precision,
    uint64_t     *tids_out,
    float        *dist_out,
    uint32_t     *k_out,
    uint32_t     *latency_us_out)
{
    char   req_key[64] = "";   /* stays empty if make_shm_key fails */
    int    shm_fd   = -1;
    int    sock     = -1;
    int    reply_fd = -1;
    void  *rmem     = MAP_FAILED;
    size_t rtotal   = 0;
    int    rc       = CUVS_STATUS_ERROR;

    if (k_out)          *k_out = 0;
    if (latency_us_out) *latency_us_out = 0;

    if (make_shm_key(req_key, sizeof(req_key)) != 0)   /* #87 finding 2 */
        goto cleanup;
    shm_fd = shm_write_query_batch(req_key, queries, n_queries, dim);
    if (shm_fd < 0)
        goto cleanup;       /* our-side error -> CUVS_STATUS_ERROR */

    /* A large batch can take longer than a single search; use a generous
     * timeout (mirrors export_adjacency). */
    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
    {
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    CuvsCmdFrame cmd = {
        .op              = CUVS_OP_SEARCH_BATCH,
        .db_oid          = db_oid,
        .index_oid       = index_oid,
        .k               = (uint32_t)k,
        .metric          = metric,
        .dim             = (uint32_t)dim,
        .n_vecs          = (int64_t)n_queries,   /* SEARCH_BATCH: Q query count */
        .shard_overfetch = shard_overfetch,
        .parallel_fanout = (uint32_t)(parallel_fanout ? 1 : 0),
        .search_mode     = search_mode,
        .bf_precision    = bf_precision,
    };
    strncpy(cmd.shm_key, req_key, sizeof(cmd.shm_key) - 1);
    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* #165: the reply segment arrives as an SCM_RIGHTS fd; its name is already
     * unlinked daemon-side, so there is nothing to open or reap here. */
    CuvsReplyHeader hdr;
    if (cuvs_fd_recv(sock, &hdr, sizeof(hdr), &reply_fd) < 0)
        goto cleanup;
    rc = (int)hdr.status;
    if (latency_us_out)
        *latency_us_out = hdr.latency_us;
    if (hdr.status != CUVS_STATUS_OK)
        goto cleanup;

    /* Daemon packs: n_results=Q, delta_merged=K (per-query result count).
     * error[] holds the former shm name for log correlation only. */
    uint32_t Q = hdr.n_results;
    uint32_t K = hdr.delta_merged;
    if (Q != n_queries || K == 0 || (int)K > k)
    {
        rc = CUVS_STATUS_ERROR;
        goto cleanup;
    }
    if (reply_fd < 0)
    {
        LOG_ERROR("[batch_search] no SCM_RIGHTS fd in reply — daemon and "
                  "extension versions are out of step (#165); restart the "
                  "pg_cuvs daemon after installing the extension\n");
        rc = CUVS_STATUS_ERROR;
        goto cleanup;
    }

    /* reply shm layout: [uint32 Q][uint32 K][tids Q*K u64][dists Q*K f32] */
    size_t hdr_bytes  = 2 * sizeof(uint32_t);
    size_t tids_bytes = (size_t)Q * (size_t)K * sizeof(uint64_t);
    size_t dist_bytes = (size_t)Q * (size_t)K * sizeof(float);
    rtotal = hdr_bytes + tids_bytes + dist_bytes;

    if (!shm_check_daemon_owner(sock, reply_fd))   /* #87 finding 1 */
        goto cleanup;
    rmem = mmap(NULL, rtotal, PROT_READ, MAP_SHARED, reply_fd, 0);
    if (rmem == MAP_FAILED)
        goto cleanup;

    {
        const char *p = (const char *)rmem + hdr_bytes;
        memcpy(tids_out, p,              tids_bytes);
        memcpy(dist_out, p + tids_bytes, dist_bytes);
    }
    if (k_out)
        *k_out = K;
    rc = CUVS_STATUS_OK;

cleanup:
    /* #165: no shm_unlink for the reply — the daemon unlinked its name before
     * replying. req_key is ours (same uid), so that one still unlinks here. */
    if (rmem != MAP_FAILED) munmap(rmem, rtotal);
    if (reply_fd >= 0)      close(reply_fd);
    if (shm_fd >= 0)        close(shm_fd);
    if (req_key[0])
        shm_unlink(req_key);
    if (sock >= 0)          close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_stats
 * ---------------------------------------------------------------- */
int
cuvs_ipc_stats(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    CuvsIndexStats *out,
    int             max,
    int            *n_out)
{
    int sock = -1;
    int rc   = CUVS_STATUS_ERROR;

    if (n_out) *n_out = 0;

    sock = uds_connect(socket_path);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down -> caller treats as empty */

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_STATUS,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;
    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n = (int)hdr.n_results;
        int n_copy = (n > max) ? max : n;
        CuvsIndexStats *buf = malloc((size_t)n * sizeof(CuvsIndexStats));
        if (!buf)
        {
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        if (recv_all(sock, buf, (size_t)n * sizeof(CuvsIndexStats)) < 0)
        {
            free(buf);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        for (int i = 0; i < n_copy; i++)
            out[i] = buf[i];
        if (n_out) *n_out = n_copy;
        free(buf);
    }

cleanup:
    if (sock >= 0)
        close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_cache_stats
 * ---------------------------------------------------------------- */
int
cuvs_ipc_cache_stats(const char *socket_path, CuvsCacheStats *out,
                     int max, int *n_out)
{
    int sock = uds_connect(socket_path);
    int rc;
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr;

    *n_out = 0;
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op = CUVS_OP_CACHE_STATS;

    rc = CUVS_STATUS_ERROR;
    if (send_cmd(sock, &cmd) == 0
        && recv_reply(sock, &hdr) == 0)
    {
        rc = (int) hdr.status;
        if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
        {
            int n = (int)hdr.n_results;
            if (n > max) n = max;
            if (recv_all(sock, out, (size_t)n * sizeof(*out)) < 0)
                rc = CUVS_STATUS_ERROR;
            else
                *n_out = n;
            /* drain any extra rows beyond max */
            for (int i = n; i < (int)hdr.n_results; i++)
            {
                CuvsCacheStats discard;
                recv_all(sock, &discard, sizeof(discard));
            }
        }
    }

    close(sock);
    return rc;
}

int
cuvs_ipc_server_settings(const char *socket_path, CuvsServerSettings *out)
{
    int sock = uds_connect_ex(socket_path, 1);
    int rc = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd = {.op = CUVS_OP_SERVER_SETTINGS};
    CuvsReplyHeader hdr;
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;
    if (send_cmd(sock, &cmd) == 0 && recv_reply(sock, &hdr) == 0)
    {
        rc = (int) hdr.status;
        if (rc == CUVS_STATUS_OK &&
            (hdr.n_results != 1 || recv_all(sock, out, sizeof(*out)) < 0))
            rc = CUVS_STATUS_ERROR;
    }
    close(sock);
    return rc;
}

int
cuvs_ipc_rmm_pool_status(const char *socket_path, CuvsRmmPoolStatus *out)
{
    int sock = uds_connect_ex(socket_path, 1);
    int rc = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd = {.op = CUVS_OP_RMM_POOL_STATUS};
    CuvsReplyHeader hdr;
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;
    memset(out, 0, sizeof(*out));
    if (send_cmd(sock, &cmd) == 0 && recv_reply(sock, &hdr) == 0)
    {
        rc = (int)hdr.status;
        if (rc == CUVS_STATUS_OK &&
            (hdr.n_results != 1 || recv_all(sock, out, sizeof(*out)) < 0 ||
             out->version != CUVS_RMM_POOL_STATUS_VERSION ||
             out->n_devices > CUVS_MAX_GPUS || out->lw_n_devices > CUVS_MAX_GPUS ||
             out->n_builds > CUVS_MAX_ACTIVE_BUILDS))
            rc = CUVS_STATUS_ERROR;
    }
    close(sock);
    return rc;
}

/* I3-C: read-only pre-spool hnsw route forecast (op 40). A short connect
 * timeout keeps CREATE INDEX on the post-scan path when the daemon is slow;
 * every failure here only means "no forecast", never a route decision. */
int
cuvs_ipc_hnsw_route_preflight(const char *socket_path,
    const CuvsHnswPreflightRequest *request, CuvsHnswPreflightReply *reply)
{
    int sock, rc = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd = {.op = CUVS_OP_HNSW_ROUTE_PREFLIGHT};
    memset(reply, 0, sizeof(*reply));
    if (!request || request->version != CUVS_HNSW_PREFLIGHT_VERSION || !request->n_estimate)
        return CUVS_STATUS_ERROR;
    sock = uds_connect_ex(socket_path, 1);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;
    if (send_cmd(sock, &cmd) == 0 && send_all(sock, request, sizeof(*request)) == 0
        && recv_all(sock, reply, sizeof(*reply)) == 0)
    {
        rc = (int)reply->header.status;
        reply->header.error[sizeof(reply->header.error) - 1] = '\0';
        reply->index_dir[sizeof(reply->index_dir) - 1] = '\0';
        if (reply->version != CUVS_HNSW_PREFLIGHT_VERSION
            || reply->n_estimate != request->n_estimate
            || (rc == CUVS_STATUS_OK && (!cuvs_hnsw_forecast_matches(request->build_algo,
                                                                      reply->route, reply->overflowed)
                                         || !reply->disk_ok)))
            rc = CUVS_STATUS_ERROR;
    }
    close(sock);
    return rc;
}

int
cuvs_ipc_cagra_ar0_preflight(const char *socket_path, CuvsCagraPreflightReply *out)
{
    int sock = uds_connect_ex(socket_path, 1);
    int rc = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd = {.op = CUVS_OP_CAGRA_AR0_PREFLIGHT};
    CuvsReplyHeader hdr;
    memset(out, 0, sizeof(*out));
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;
    if (send_cmd(sock, &cmd) == 0 && recv_all(sock, &hdr, sizeof(hdr)) == 0)
    {
        rc = (int)hdr.status;
        if (rc == CUVS_STATUS_OK
            && (hdr.n_results != 1 || recv_all(sock, out, sizeof(*out)) < 0
                || out->version != CUVS_CAGRA_PREFLIGHT_VERSION
                || out->n_devices > CUVS_MAX_GPUS))
            rc = CUVS_STATUS_ERROR;
    }
    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_shard_stats (Phase 3F)
 * ---------------------------------------------------------------- */
int
cuvs_ipc_shard_stats(const char *socket_path, uint32_t db_oid,
                     uint32_t index_oid, CuvsShardStats *out,
                     int max, int *n_out)
{
    int sock = -1;
    int rc   = CUVS_STATUS_ERROR;

    if (n_out) *n_out = 0;

    sock = uds_connect(socket_path);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down -> caller treats as empty */

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_SHARD_STATS,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;
    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n      = (int)hdr.n_results;
        int n_copy = (n > max) ? max : n;
        for (int i = 0; i < n; i++)
        {
            CuvsShardStats row;
            if (recv_all(sock, &row, sizeof(row)) < 0)
            {
                rc = CUVS_STATUS_ERROR;
                if (n_out) *n_out = 0;
                goto cleanup;
            }
            if (i < n_copy)
                out[i] = row;       /* extra rows beyond max are drained */
        }
        if (n_out) *n_out = n_copy;
    }

cleanup:
    if (sock >= 0)
        close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_mark_stale
 * ---------------------------------------------------------------- */
int
cuvs_ipc_mark_stale(const char *socket_path, uint32_t db_oid, uint32_t index_oid)
{
    int sock = uds_connect(socket_path);
    int rc;
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr;

    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down — do not fail the write */

    memset(&cmd, 0, sizeof(cmd));
    cmd.op        = CUVS_OP_MARK_STALE;
    cmd.db_oid    = db_oid;
    cmd.index_oid = index_oid;

    rc = CUVS_STATUS_ERROR;
    if (send_cmd(sock, &cmd) == 0
        && recv_reply(sock, &hdr) == 0)
        rc = (int) hdr.status;

    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_drop (Phase 3G.1)
 * ---------------------------------------------------------------- */
int
cuvs_ipc_drop(const char *socket_path, uint32_t db_oid, uint32_t index_oid)
{
    int sock = uds_connect(socket_path);
    int rc;
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr;

    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down — caller must not fail the DROP */

    memset(&cmd, 0, sizeof(cmd));
    cmd.op        = CUVS_OP_DROP_INDEX;
    cmd.db_oid    = db_oid;
    cmd.index_oid = index_oid;

    rc = CUVS_STATUS_ERROR;
    if (send_cmd(sock, &cmd) == 0
        && recv_reply(sock, &hdr) == 0)
        rc = (int) hdr.status;

    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_build
 *
 * DUP-SIBLING(build): cuvs_ipc_build_flat is a duplicate (ADR-073). Change both
 * together; see the duplicate-pair inventory in ADR-073.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_build(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const struct CuvsBuildCorpus *corpus,
    const float   *heap_vecs,
    const uint64_t *heap_tids,
    int64_t        n_vecs,
    int            dim,
    uint32_t       metric,
    const char    *index_dir,
    uint32_t       table_oid,
    uint32_t       relfilenode,
    uint32_t       shard_count,
    uint32_t       use_cpu_hnsw,
    uint32_t       graph_degree,
    uint32_t       intermediate_graph_degree,
    uint32_t       build_algo,
    const CuvsPageBuildRequest *page_build,
    uint64_t       *graph_nonce_out)
{
    char shm_key[64] = "";
    int  legacy_shm_fd = -1;   /* CORPUS_HEAP only: created + unlinked here */
    int  pass_fd = -1;         /* CORPUS_MEMFD only: passed via SCM_RIGHTS */
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd;
    char dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    /* Stage the payload by tier. */
    if (corpus->kind == CORPUS_MEMFD)
    {
        pass_fd = corpus->fd;                 /* anonymous; daemon mmaps the fd */
    }
    else if (corpus->kind == CORPUS_SHM)
    {
        strncpy(shm_key, corpus->shm_name, sizeof(shm_key) - 1);  /* daemon shm_open by name */
    }
    else  /* CORPUS_HEAP: copy into a fresh named shm, unlinked at cleanup */
    {
        if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
            goto cleanup;
        legacy_shm_fd = shm_write_build_payload(shm_key, heap_vecs, heap_tids, n_vecs, dim);
        if (legacy_shm_fd < 0) {
            LOG_ERROR("[cuvs_ipc_build] shm_write FAILED errno=%d (%s)\n",
                    errno, strerror(errno));
            goto cleanup;
        }
    }
    LOG_DEBUG("[cuvs_ipc_build] tier=%d shm_key=%s pass_fd=%d socket=%s n_vecs=%lld dim=%d\n",
        (int)corpus->kind, shm_key, pass_fd, socket_path, (long long)n_vecs, dim);

    sock = uds_connect_ex(socket_path, 600);  /* BUILD can take minutes */
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_build] uds_connect FAILED errno=%d (%s)\n",
                errno, strerror(errno));
        rc = CUVS_STATUS_UNAVAILABLE;   /* daemon not reachable */
        goto cleanup;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.op           = page_build ? CUVS_OP_BUILD_PAGES : CUVS_OP_BUILD;
    cmd.db_oid       = db_oid;
    cmd.index_oid    = index_oid;
    cmd.metric       = metric;
    cmd.dim          = (uint32_t)dim;
    cmd.n_vecs       = n_vecs;
    cmd.table_oid    = table_oid;
    cmd.relfilenode  = relfilenode;
    cmd.shard_count  = shard_count;
    cmd.use_cpu_hnsw = use_cpu_hnsw;  /* Phase 3I-1: 1 = serialize .hnsw sidecar */
    cmd.graph_degree              = graph_degree;              /* 3R; 0 = cuVS default */
    cmd.intermediate_graph_degree = intermediate_graph_degree; /* 3R; 0 = cuVS default */
    cmd.build_algo                = build_algo;                /* 3R; 0 = AUTO */
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);  /* "" for memfd tier */

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    if (page_build && send_all(sock, page_build, sizeof(*page_build)) < 0)
        goto cleanup;

    /* index_dir (fixed 256B) carries the SCM_RIGHTS fd for the memfd tier; for
     * shm/heap pass_fd is -1 and only the bytes are sent. Daemon recvmsg's both. */
    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, pass_fd, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;
    /* A zero nonce means BUILD accepted page storage without a searchable graph. */
    if (page_build && rc == CUVS_STATUS_OK &&
        recv_all(sock, graph_nonce_out, sizeof(*graph_nonce_out)) < 0)
        rc = CUVS_STATUS_ERROR;

cleanup:
    if (sock >= 0)
        close(sock);
    /* Only the legacy heap-tier shm is owned here; the memfd/shm corpus lifetime
     * belongs to the caller (released via cuvs_corpus_close). */
    if (legacy_shm_fd >= 0)
    {
        close(legacy_shm_fd);
        shm_unlink(shm_key);
    }
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_build_flat (ADR-073)
 *
 * DUP-SIBLING(build). Duplicate of cuvs_ipc_build (regression safety — the plan
 * mandates duplicate-not-refactor for flat) that sends CUVS_OP_BUILD_FLAT. The corpus
 * staging by tier is identical; only the op code and the (absent) graph params
 * differ. The daemon's handle_build_flat persists .tids + .vectors and registers
 * a brute-force-only entry — it never calls cuvs_cagra_build.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_build_flat(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const struct CuvsBuildCorpus *corpus,
    const float   *heap_vecs,
    const uint64_t *heap_tids,
    int64_t        n_vecs,
    int            dim,
    uint32_t       metric,
    const char    *index_dir,
    uint32_t       table_oid,
    uint32_t       relfilenode)
{
    char shm_key[64] = "";
    int  legacy_shm_fd = -1;   /* CORPUS_HEAP only: created + unlinked here */
    int  pass_fd = -1;         /* CORPUS_MEMFD only: passed via SCM_RIGHTS */
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd;
    char dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    /* Stage the payload by tier (identical to cuvs_ipc_build). */
    if (corpus->kind == CORPUS_MEMFD)
    {
        pass_fd = corpus->fd;                 /* anonymous; daemon mmaps the fd */
    }
    else if (corpus->kind == CORPUS_SHM)
    {
        strncpy(shm_key, corpus->shm_name, sizeof(shm_key) - 1);  /* daemon shm_open by name */
    }
    else  /* CORPUS_HEAP: copy into a fresh named shm, unlinked at cleanup */
    {
        if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
            goto cleanup;
        legacy_shm_fd = shm_write_build_payload(shm_key, heap_vecs, heap_tids, n_vecs, dim);
        if (legacy_shm_fd < 0) {
            LOG_ERROR("[cuvs_ipc_build_flat] shm_write FAILED errno=%d (%s)\n",
                    errno, strerror(errno));
            goto cleanup;
        }
    }
    LOG_DEBUG("[cuvs_ipc_build_flat] tier=%d shm_key=%s pass_fd=%d socket=%s n_vecs=%lld dim=%d\n",
        (int)corpus->kind, shm_key, pass_fd, socket_path, (long long)n_vecs, dim);

    sock = uds_connect_ex(socket_path, 600);  /* BUILD can take a while */
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_build_flat] uds_connect FAILED errno=%d (%s)\n",
                errno, strerror(errno));
        rc = CUVS_STATUS_UNAVAILABLE;   /* daemon not reachable */
        goto cleanup;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.op           = CUVS_OP_BUILD_FLAT;
    cmd.db_oid       = db_oid;
    cmd.index_oid    = index_oid;
    cmd.metric       = metric;
    cmd.dim          = (uint32_t)dim;
    cmd.n_vecs       = n_vecs;
    cmd.table_oid    = table_oid;
    cmd.relfilenode  = relfilenode;
    cmd.shard_count  = 1;   /* flat is always unsharded; daemon ignores this */
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);  /* "" for memfd tier */

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* index_dir (fixed 256B) carries the SCM_RIGHTS fd for the memfd tier; for
     * shm/heap pass_fd is -1 and only the bytes are sent. Daemon recvmsg's both. */
    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, pass_fd, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0)
        close(sock);
    if (legacy_shm_fd >= 0)
    {
        close(legacy_shm_fd);
        shm_unlink(shm_key);
    }
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_build_multi (ADR-059)
 *
 * Like cuvs_ipc_build but references N worker named-shm partials instead of one
 * merged corpus. Sends the cmd frame (n_partials > 0), the index_dir frame (no
 * SCM_RIGHTS fd), then the CuvsPartialDesc list. The daemon mmaps each partial
 * and streams it straight to the GPU — no host merge. The caller owns the
 * partials and unlinks them after this returns.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_build_multi(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const CuvsPartialDesc *partials,
    uint32_t       n_partials,
    int64_t        total,
    int            dim,
    uint32_t       metric,
    const char    *index_dir,
    uint32_t       table_oid,
    uint32_t       relfilenode,
    uint32_t       shard_count,
    uint32_t       use_cpu_hnsw,
    uint32_t       graph_degree,
    uint32_t       intermediate_graph_degree,
    uint32_t       build_algo)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd;
    char dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    if (n_partials == 0 || partials == NULL)
    {
        LOG_ERROR("[cuvs_ipc_build_multi] no partials (n_partials=%u)\n", n_partials);
        return CUVS_STATUS_ERROR;
    }

    sock = uds_connect_ex(socket_path, 600);  /* BUILD can take minutes */
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_build_multi] uds_connect FAILED errno=%d (%s)\n",
                errno, strerror(errno));
        return CUVS_STATUS_UNAVAILABLE;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.op           = CUVS_OP_BUILD;
    cmd.db_oid       = db_oid;
    cmd.index_oid    = index_oid;
    cmd.metric       = metric;
    cmd.dim          = (uint32_t)dim;
    cmd.n_vecs       = total;
    cmd.table_oid    = table_oid;
    cmd.relfilenode  = relfilenode;
    cmd.shard_count  = shard_count;
    cmd.use_cpu_hnsw = use_cpu_hnsw;
    cmd.n_partials   = n_partials;   /* daemon takes the multi-partial path */
    cmd.graph_degree              = graph_degree;              /* 3R */
    cmd.intermediate_graph_degree = intermediate_graph_degree; /* 3R */
    cmd.build_algo                = build_algo;                /* 3R */
    /* cmd.shm_key stays "" — no single corpus segment. */

    LOG_DEBUG("[cuvs_ipc_build_multi] n_partials=%u total=%lld dim=%d socket=%s\n",
        n_partials, (long long)total, dim, socket_path);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* index_dir frame: no SCM_RIGHTS fd for the multi-partial path. */
    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, -1, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    /* Partial descriptor list (fixed-size records). */
    if (send_all(sock, partials, (size_t)n_partials * sizeof(CuvsPartialDesc)) < 0)
        goto cleanup;

    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0)
        close(sock);
    return rc;   /* caller owns the partials' lifetime */
}

/* ----------------------------------------------------------------
 * Phase 3J: export CAGRA adjacency list + corpus vectors from daemon.
 *
 * Sends CUVS_OP_EXPORT_ADJACENCY; daemon packs adj+vecs+tids into a
 * shared memory segment and returns its key via the reply header.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_export_adjacency(
    const char  *socket_path,
    uint32_t     db_oid,
    uint32_t     index_oid,
    uint32_t   **adj_out,
    float      **vecs_out,
    uint64_t   **tids_out,
    size_t      *n_vecs_out,
    int         *graph_degree_out,
    int         *dim_out,
    uint32_t    *metric_out)
{
    int  sock = -1;
    int  shm_fd = -1;
    int  rc = CUVS_STATUS_ERROR;
    void *mem = MAP_FAILED;

    /* Long timeout: GPU→CPU copy for large indexes takes 10-30s. */
    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_EXPORT_ADJACENCY,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };
    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* #165: the segment arrives as an SCM_RIGHTS fd. The daemon unlinks the
     * name before replying, so there is nothing to open by name and nothing
     * left behind to leak. */
    CuvsReplyHeader hdr;
    if (cuvs_fd_recv(sock, &hdr, sizeof(hdr), &shm_fd) < 0)
        goto cleanup;

    if (hdr.status != CUVS_STATUS_OK) {
        rc = (int)hdr.status;
        goto cleanup;
    }

    /* Daemon packs: n_vecs=n_results, graph_degree=latency_us, dim=delta_merged.
     * error[] holds the former shm name for log correlation only. */
    size_t  n_vecs       = (size_t)hdr.n_results;
    int     graph_degree = (int)hdr.latency_us;
    int     dim          = (int)hdr.delta_merged;

    if (n_vecs == 0 || graph_degree <= 0 || dim <= 0) {
        LOG_ERROR("[export_adjacency] bad reply n=%zu gd=%d dim=%d\n",
                  n_vecs, graph_degree, dim);
        goto cleanup;
    }
    if (shm_fd < 0) {
        LOG_ERROR("[export_adjacency] no SCM_RIGHTS fd in reply — daemon and "
                  "extension versions are out of step (#165); restart the "
                  "pg_cuvs daemon after installing the extension\n");
        goto cleanup;
    }

    /* shm layout: [uint32_t n_vecs][uint32_t graph_degree][uint32_t dim][uint32_t pad]
     *             [adj: n*gd*4][vecs: n*dim*4][tids: n*8] */
    size_t adj_bytes  = n_vecs * (size_t)graph_degree * sizeof(uint32_t);
    size_t vecs_bytes = n_vecs * (size_t)dim * sizeof(float);
    size_t tids_bytes = n_vecs * sizeof(uint64_t);
    size_t hdr_bytes  = 4 * sizeof(uint32_t);  /* n_vecs, gd, dim, pad */
    size_t total      = hdr_bytes + adj_bytes + vecs_bytes + tids_bytes;

    if (!shm_check_daemon_owner(sock, shm_fd)) {   /* #87 finding 1 */
        LOG_ERROR("[export_adjacency] shm owner mismatch on passed fd\n");
        goto cleanup;
    }

    mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (mem == MAP_FAILED) {
        LOG_ERROR("[export_adjacency] mmap failed errno=%d\n", errno);
        goto cleanup;
    }

    /* Read metric from header slot [3] before the data section. */
    uint32_t src_metric = ((const uint32_t *)mem)[3];

    /* Copy out to caller-owned buffers. */
    const char *p = (const char *)mem + hdr_bytes;
    uint32_t *adj  = (uint32_t *)malloc(adj_bytes);
    float    *vecs = (float    *)malloc(vecs_bytes);
    uint64_t *tids = (uint64_t *)malloc(tids_bytes);
    if (!adj || !vecs || !tids) {
        free(adj); free(vecs); free(tids);
        LOG_ERROR("[export_adjacency] malloc failed\n");
        goto cleanup;
    }
    memcpy(adj,  p,                            adj_bytes);
    memcpy(vecs, p + adj_bytes,                vecs_bytes);
    memcpy(tids, p + adj_bytes + vecs_bytes,   tids_bytes);

    *adj_out          = adj;
    *vecs_out         = vecs;
    *tids_out         = tids;
    *n_vecs_out       = n_vecs;
    *graph_degree_out = graph_degree;
    *dim_out          = dim;
    if (metric_out) *metric_out = src_metric;
    rc = CUVS_STATUS_OK;

cleanup:
    /* #165: no shm_unlink here — the daemon already unlinked the name, and this
     * process (a different uid) could not have removed it anyway. Closing the
     * passed fd releases the last reference. */
    if (mem != MAP_FAILED)  munmap(mem, total);
    if (shm_fd >= 0)        close(shm_fd);
    if (sock >= 0)          close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Phase 3J: request daemon to run from_cagra() on a loaded CAGRA index
 * and serialize the resulting multi-level HNSW to /dev/shm (no disk I/O).
 * The path is returned in shm_path_out; caller must unlink after reading.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_export_hnsw_shm(
    const char *socket_path,
    uint32_t    db_oid,
    uint32_t    index_oid,
    char       *shm_path_out,
    size_t      shm_path_len,
    int        *fd_out)
{
    int sock    = -1;
    int rc      = CUVS_STATUS_ERROR;
    int pass_fd = -1;

    if (fd_out) *fd_out = -1;

    /* from_cagra() can take 30s+ for large indexes */
    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_EXPORT_HNSW_SHM,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };
    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* #165: the sidecar arrives as an SCM_RIGHTS fd, already unlinked daemon-side. */
    CuvsReplyHeader hdr;
    if (cuvs_fd_recv(sock, &hdr, sizeof(hdr), &pass_fd) < 0)
        goto cleanup;

    if (hdr.status != CUVS_STATUS_OK) {
        rc = (int)hdr.status;
        goto cleanup;
    }

    if (pass_fd < 0) {
        LOG_ERROR("[export_hnsw_shm] no SCM_RIGHTS fd in reply -- daemon and "
                  "extension versions are out of step (#165); restart the "
                  "pg_cuvs daemon after installing the extension\n");
        goto cleanup;
    }
    if (!shm_check_daemon_owner(sock, pass_fd)) {   /* #87 finding 1 */
        LOG_ERROR("[export_hnsw_shm] sidecar owner mismatch on passed fd\n");
        goto cleanup;
    }

    /*
     * #166 review: do NOT hand back a "/proc/self/fd/N" path. Opening that
     * symlink re-checks inode permissions, so a daemon running with a hardened
     * umask (the segment lands 0600, daemon-owned) makes the cross-uid reopen
     * fail EACCES even though reads through the passed descriptor itself are
     * fine. The importer consumes the descriptor directly; the path is only
     * carried for diagnostics.
     */
    strncpy(shm_path_out, hdr.error, shm_path_len - 1);
    shm_path_out[shm_path_len - 1] = '\0';
    if (fd_out) *fd_out = pass_fd;
    pass_fd = -1;                  /* ownership transferred to the caller */
    rc = CUVS_STATUS_OK;

cleanup:
    if (pass_fd >= 0) close(pass_fd);
    if (sock >= 0)    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * CUVS_OP_EXPORT_HNSW_UPPER — the hnswlib file and the TID file of a
 * loaded CAGRA index, each as a daemon-owned memfd (see cuvs_ipc.h).
 * ---------------------------------------------------------------- */
int
cuvs_ipc_export_hnsw_upper(
    const char  *socket_path,
    uint32_t     db_oid,
    uint32_t     index_oid,
    int         *hnsw_fd_out,
    int         *tids_fd_out,
    size_t      *n_vecs_out,
    int         *degree_out,
    int         *dim_out)
{
    int  sock = -1, hnsw_fd = -1, tids_fd = -1;
    int  rc = CUVS_STATUS_ERROR;
    char token;

    *hnsw_fd_out = *tids_fd_out = -1;
    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_EXPORT_HNSW_UPPER,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };
    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* The daemon replies only after building the upper layers, which grows
     * with the index and may take longer than any fixed receive timeout. Wait
     * for the reply without one, in 250 ms slices that ask the wait callback
     * (query cancel, statement_timeout): on a cancel the socket is closed,
     * which the daemon takes as the signal to stop building. A daemon that
     * exits closes the socket as well and ends the wait. */
    for (;;)
    {
        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 250);

        if (pr > 0)
            break;
        if (pr < 0 && errno != EINTR)
            goto cleanup;
        if (g_wait_cb && g_wait_cb())
        {
            rc = CUVS_STATUS_CANCELED;
            goto cleanup;
        }
    }

    CuvsReplyHeader hdr;
    if (cuvs_fd_recv(sock, &hdr, sizeof(hdr), &hnsw_fd) < 0)
        goto cleanup;
    if (hdr.status != CUVS_STATUS_OK) {
        rc = (int)hdr.status;
        goto cleanup;
    }
    if (hnsw_fd < 0 || cuvs_fd_recv(sock, &token, 1, &tids_fd) < 0 || tids_fd < 0) {
        LOG_ERROR("[export_hnsw_upper] reply without its two SCM_RIGHTS fds\n");
        goto cleanup;
    }
    if (!shm_check_daemon_owner(sock, hnsw_fd) || !shm_check_daemon_owner(sock, tids_fd)) {
        LOG_ERROR("[export_hnsw_upper] owner mismatch on a passed fd\n");
        goto cleanup;
    }
    if (hdr.n_results == 0 || hdr.latency_us == 0 || hdr.delta_merged == 0) {
        LOG_ERROR("[export_hnsw_upper] bad reply n=%u degree=%u dim=%u\n",
                  hdr.n_results, hdr.latency_us, hdr.delta_merged);
        goto cleanup;
    }
    *n_vecs_out  = (size_t)hdr.n_results;
    *degree_out  = (int)hdr.latency_us;
    *dim_out     = (int)hdr.delta_merged;
    *hnsw_fd_out = hnsw_fd;
    *tids_fd_out = tids_fd;
    hnsw_fd = tids_fd = -1;        /* ownership transferred to the caller */
    rc = CUVS_STATUS_OK;

cleanup:
    if (hnsw_fd >= 0) close(hnsw_fd);
    if (tids_fd >= 0) close(tids_fd);
    if (sock >= 0)    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search_filtered (D-wedge spike)
 *
 * Thin wrapper around cuvs_ipc_search that adds a second shm segment
 * carrying the filter TID array.  The daemon post-filters BF results
 * to only TIDs present in the set.
 *
 * When filter_tids is NULL or n_filter == 0 this degrades to the
 * standard unfiltered BF path (n_filter_tids=0 in the frame).
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search_filtered(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const float   *query_vec,
    int            dim,
    int            k,
    uint32_t       metric,
    uint32_t       search_mode,
    uint32_t       bf_precision,
    const uint64_t *filter_tids,
    uint32_t       n_filter,
    uint64_t      *tids_out,
    float         *dist_out,
    int           *n_out,
    uint32_t      *latency_us_out,
    int           *delta_merged_out,
    int            use_prefilter,
    int            exact_chunk_cap)
{
    char shm_key[64] = "";   /* stays empty if make_shm_key fails */
    char filter_shm_key[64] = "";
    int  shm_fd        = -1;
    int  filter_shm_fd = -1;
    int  sock          = -1;
    int  rc            = CUVS_STATUS_ERROR;
    int  has_filter    = (filter_tids != NULL && n_filter > 0);

    if (latency_us_out)   *latency_us_out   = 0;
    if (n_out)            *n_out            = 0;
    if (delta_merged_out) *delta_merged_out = 0;

    /* --- query shm --- */
    if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
        goto cleanup;
    shm_fd = shm_write_query(shm_key, query_vec, dim);
    if (shm_fd < 0)
        goto cleanup;

    /* --- filter shm (flat uint64_t[n_filter]) --- */
    if (has_filter)
    {
        if (make_shm_key(filter_shm_key, sizeof(filter_shm_key)) != 0)   /* #87 finding 2 */
            goto cleanup;
        size_t filter_bytes = (size_t)n_filter * sizeof(uint64_t);

        filter_shm_fd = shm_open(filter_shm_key, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (filter_shm_fd < 0)
            goto cleanup;
        fchmod(filter_shm_fd, 0644);   /* guarantee daemon (other uid) read access; see above (#87) */
        if (ftruncate(filter_shm_fd, (off_t)filter_bytes) < 0)
        {
            shm_unlink(filter_shm_key);
            goto cleanup;
        }
        void *fmem = mmap(NULL, filter_bytes, PROT_READ | PROT_WRITE,
                          MAP_SHARED, filter_shm_fd, 0);
        if (fmem == MAP_FAILED)
        {
            shm_unlink(filter_shm_key);
            goto cleanup;
        }
        memcpy(fmem, filter_tids, filter_bytes);
        cuvs_u64_sort((uint64_t *) fmem, n_filter);
        munmap(fmem, filter_bytes);
    }
    else
    {
        filter_shm_key[0] = '\0';
    }

    sock = uds_connect(socket_path);
    if (sock < 0)
    {
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    CuvsCmdFrame cmd = {
        .op            = CUVS_OP_SEARCH,
        .db_oid        = db_oid,
        .index_oid     = index_oid,
        .k             = (uint32_t)k,
        .metric        = metric,
        .dim           = (uint32_t)dim,
        .n_vecs        = (int64_t)(exact_chunk_cap > 0 ? exact_chunk_cap : 1),
        .search_mode   = search_mode,
        .bf_precision  = bf_precision,
        .n_filter_tids = has_filter ? n_filter : 0,
        .use_prefilter = has_filter ? (uint32_t)use_prefilter : 0,
    };
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);
    if (has_filter)
        strncpy(cmd.filter_shm_key, filter_shm_key,
                sizeof(cmd.filter_shm_key) - 1);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    {
        int wr = recv_reply_interruptible(sock, &hdr, 30);
        if (wr == 1) { rc = CUVS_STATUS_CANCELED; goto cleanup; }
        if (wr < 0)  goto cleanup;
    }

    rc = (int)hdr.status;
    if (latency_us_out)   *latency_us_out   = hdr.latency_us;
    if (delta_merged_out) *delta_merged_out = (int) hdr.delta_merged;

    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n       = (int)hdr.n_results;
        int n_write = (n > k) ? k : n;
        CuvsResult *results = malloc(n * sizeof(CuvsResult));
        if (!results)
        {
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        if (recv_all(sock, results, n * sizeof(CuvsResult)) < 0)
        {
            free(results);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        for (int i = 0; i < n_write; i++)
        {
            tids_out[i] = results[i].tid;
            dist_out[i] = results[i].distance;
        }
        *n_out = n_write;
        free(results);
    }
    else
    {
        *n_out = 0;
    }

cleanup:
    if (sock >= 0)        close(sock);
    if (shm_fd >= 0)      close(shm_fd);
    if (filter_shm_fd >= 0) close(filter_shm_fd);
    if (shm_key[0])
        shm_unlink(shm_key);
    if (has_filter && filter_shm_key[0])
        shm_unlink(filter_shm_key);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search_stream_bf (ADR-064)
 *
 * Out-of-core filtered BF. Mirrors cuvs_ipc_search_filtered's query+filter shm
 * packing, but op=CUVS_OP_SEARCH_STREAM_BF and cmd.n_vecs carries the per-chunk
 * cap. Exact (search_mode=brute_force, float32, no overfetch). A non-empty
 * filter is required — an empty filter has nothing to gather.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search_stream_bf(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    const float    *query_vec,
    int             dim,
    int             k,
    uint32_t        metric,
    const uint64_t *filter_tids,
    uint32_t        n_filter,
    int             chunk_cap,
    uint64_t       *tids_out,
    float          *dist_out,
    int            *n_out,
    uint32_t       *latency_us_out)
{
    char shm_key[64] = "";   /* stays empty if make_shm_key fails */
    char filter_shm_key[64] = "";
    int  shm_fd        = -1;
    int  filter_shm_fd = -1;
    int  sock          = -1;
    int  rc            = CUVS_STATUS_ERROR;
    int  has_filter    = (filter_tids != NULL && n_filter > 0);

    if (latency_us_out) *latency_us_out = 0;
    if (n_out)          *n_out          = 0;

    if (!has_filter)
        return CUVS_STATUS_NO_VECTORS;   /* nothing to gather */

    /* --- query shm --- */
    if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
        goto cleanup;
    shm_fd = shm_write_query(shm_key, query_vec, dim);
    if (shm_fd < 0)
        goto cleanup;

    /* --- filter shm (flat uint64_t[n_filter]) --- */
    {
        if (make_shm_key(filter_shm_key, sizeof(filter_shm_key)) != 0)   /* #87 finding 2 */
            goto cleanup;
        size_t filter_bytes = (size_t)n_filter * sizeof(uint64_t);

        filter_shm_fd = shm_open(filter_shm_key, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (filter_shm_fd < 0)
            goto cleanup;
        fchmod(filter_shm_fd, 0644);   /* guarantee daemon (other uid) read access; see above (#87) */
        if (ftruncate(filter_shm_fd, (off_t)filter_bytes) < 0)
        {
            shm_unlink(filter_shm_key);
            goto cleanup;
        }
        void *fmem = mmap(NULL, filter_bytes, PROT_READ | PROT_WRITE,
                          MAP_SHARED, filter_shm_fd, 0);
        if (fmem == MAP_FAILED)
        {
            shm_unlink(filter_shm_key);
            goto cleanup;
        }
        memcpy(fmem, filter_tids, filter_bytes);
        cuvs_u64_sort((uint64_t *) fmem, n_filter);
        munmap(fmem, filter_bytes);
    }

    sock = uds_connect(socket_path);
    if (sock < 0)
    {
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    CuvsCmdFrame cmd = {
        .op            = CUVS_OP_SEARCH_STREAM_BF,
        .db_oid        = db_oid,
        .index_oid     = index_oid,
        .k             = (uint32_t)k,
        .metric        = metric,
        .dim           = (uint32_t)dim,
        .n_vecs        = (int64_t)(chunk_cap > 0 ? chunk_cap : 1),  /* per-chunk cap */
        .search_mode   = 1,   /* brute_force */
        .n_filter_tids = n_filter,
    };
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);
    strncpy(cmd.filter_shm_key, filter_shm_key, sizeof(cmd.filter_shm_key) - 1);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    {
        int wr = recv_reply_interruptible(sock, &hdr, 30);
        if (wr == 1) { rc = CUVS_STATUS_CANCELED; goto cleanup; }
        if (wr < 0)  goto cleanup;
    }

    rc = (int)hdr.status;
    if (latency_us_out) *latency_us_out = hdr.latency_us;

    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n       = (int)hdr.n_results;
        int n_write = (n > k) ? k : n;
        CuvsResult *results = malloc(n * sizeof(CuvsResult));
        if (!results)
        {
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        if (recv_all(sock, results, n * sizeof(CuvsResult)) < 0)
        {
            free(results);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        for (int i = 0; i < n_write; i++)
        {
            tids_out[i] = results[i].tid;
            dist_out[i] = results[i].distance;
        }
        *n_out = n_write;
        free(results);
    }
    else
    {
        *n_out = 0;
    }

cleanup:
    if (sock >= 0)          close(sock);
    if (shm_fd >= 0)        close(shm_fd);
    if (filter_shm_fd >= 0) close(filter_shm_fd);
    if (shm_key[0])
        shm_unlink(shm_key);
    if (has_filter && filter_shm_key[0])
        shm_unlink(filter_shm_key);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search_bf_transient (ADR-073 B)
 *
 * One-shot GPU exact BF over a backend-supplied per-query corpus (no resident
 * index). Stages [vecs][tids] into a memfd (SCM_RIGHTS) or named-shm fallback
 * (the same CuvsBuildCorpus handoff the build path uses), puts the query in a
 * separate shm, and sends CUVS_OP_SEARCH_BF_TRANSIENT. The corpus name (shm
 * tier) rides in filter_shm_key; the memfd fd rides on the second frame's
 * SCM_RIGHTS, exactly like cuvs_ipc_build's index_dir+fd. Reply = header +
 * n_results × CuvsResult (same as search). No CPU fallback — any non-OK is fatal.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search_bf_transient(
    const char     *socket_path,
    uint32_t        db_oid,
    const float    *corpus_vecs,
    const uint64_t *corpus_tids,
    int64_t         n,
    const float    *query_vec,
    int             dim,
    int             k,
    uint32_t        metric,
    uint64_t       *tids_out,
    float          *dist_out,
    int            *n_out,
    uint32_t       *latency_us_out,
    char           *err_out,
    size_t          err_len)
{
    char shm_key[64] = "";
    int  shm_fd      = -1;
    int  sock        = -1;
    int  rc          = CUVS_STATUS_ERROR;
    int  pass_fd     = -1;
    CuvsBuildCorpus corpus = {0};
    char dir_buf[256] = {0};   /* unused payload; carries the SCM_RIGHTS fd only */

    if (latency_us_out) *latency_us_out = 0;
    if (n_out)          *n_out          = 0;
    if (err_out && err_len) err_out[0]  = '\0';

    if (n < 1 || dim <= 0 || k < 1)
        return CUVS_STATUS_ERROR;

    /* Overflow guard mirrors the daemon build path. */
    {
        size_t per_vec = (size_t)dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)n > SIZE_MAX / per_vec)
            return CUVS_STATUS_ERROR;
    }

    size_t vec_bytes = (size_t)n * (size_t)dim * sizeof(float);
    size_t tid_bytes = (size_t)n * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    /* --- Stage the corpus (memfd default, shm fallback). --- */
    if (cuvs_corpus_open(&corpus, total) != 0 || corpus.base == NULL)
    {
        /* CORPUS_HEAP (no working shared memory) cannot hand a corpus to the
         * daemon — transient BF is unavailable in that degenerate environment. */
        if (err_out && err_len)
            snprintf(err_out, err_len, "no shared-memory tier for transient BF corpus");
        cuvs_corpus_close(&corpus);
        return CUVS_STATUS_ERROR;
    }
    memcpy(corpus.base, corpus_vecs, vec_bytes);
    memcpy((char *)corpus.base + vec_bytes, corpus_tids, tid_bytes);
    if (corpus.kind == CORPUS_MEMFD)
        pass_fd = corpus.fd;   /* anonymous; daemon mmaps via SCM_RIGHTS */

    /* --- Query shm. --- */
    if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
        goto cleanup;
    shm_fd = shm_write_query(shm_key, query_vec, dim);
    if (shm_fd < 0)
        goto cleanup;

    sock = uds_connect(socket_path);
    if (sock < 0)
    {
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    CuvsCmdFrame cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op       = CUVS_OP_SEARCH_BF_TRANSIENT;
    cmd.db_oid   = db_oid;
    cmd.k        = (uint32_t)k;
    cmd.metric   = metric;
    cmd.dim      = (uint32_t)dim;
    cmd.n_vecs   = n;
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);  /* query */
    if (corpus.kind == CORPUS_SHM)                            /* corpus (shm tier) */
        strncpy(cmd.filter_shm_key, corpus.shm_name, sizeof(cmd.filter_shm_key) - 1);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    /* Second frame carries the corpus memfd via SCM_RIGHTS (pass_fd=-1 for the
     * shm tier — the daemon recvmsg's it either way, exactly like handle_build). */
    if (cuvs_fd_send(sock, pass_fd, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    {
        int wr = recv_reply_interruptible(sock, &hdr, 30);
        if (wr == 1) { rc = CUVS_STATUS_CANCELED; goto cleanup; }
        if (wr < 0)  goto cleanup;
    }

    rc = (int)hdr.status;
    if (latency_us_out) *latency_us_out = hdr.latency_us;
    if (rc != CUVS_STATUS_OK && err_out && err_len)
    {
        strncpy(err_out, hdr.error, err_len - 1);
        err_out[err_len - 1] = '\0';
    }

    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n_res   = (int)hdr.n_results;
        int n_write = (n_res > k) ? k : n_res;
        CuvsResult *results = malloc((size_t)n_res * sizeof(CuvsResult));
        if (!results)
        {
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        if (recv_all(sock, results, (size_t)n_res * sizeof(CuvsResult)) < 0)
        {
            free(results);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        for (int i = 0; i < n_write; i++)
        {
            tids_out[i] = results[i].tid;
            dist_out[i] = results[i].distance;
        }
        if (n_out) *n_out = n_write;
        free(results);
    }

cleanup:
    if (sock >= 0)   close(sock);
    if (shm_fd >= 0) close(shm_fd);
    shm_unlink(shm_key);
    /* Close AFTER the reply: the shm-tier daemon opens the corpus by name during
     * handling, so unlinking earlier would race it. cuvs_corpus_close munmaps +
     * closes (+ shm_unlinks for the shm tier); the memfd's daemon-side mapping
     * survives our close. */
    cuvs_corpus_close(&corpus);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_build_ivfpq (3P)
 *
 * Sends a BUILD_IVFPQ request. Corpus is written to a named shm segment
 * (heap tier only — memfd/parallel corpus not needed for initial release).
 * n_lists/pq_bits/pq_dim: 0 lets the daemon apply defaults (1024/8/ceil(dim/2)).
 * ---------------------------------------------------------------- */
int
cuvs_ipc_build_ivfpq(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    const float    *vecs,
    const uint64_t *tids,
    int64_t         n_vecs,
    int             dim,
    uint32_t        metric,
    const char     *index_dir,
    uint32_t        table_oid,
    uint32_t        relfilenode,
    uint32_t        n_lists,
    uint32_t        pq_bits,
    uint32_t        pq_dim)
{
    char shm_key[64] = "";
    int  shm_fd  = -1;
    int  sock    = -1;
    int  rc      = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    char            dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
        goto cleanup;
    shm_fd = shm_write_build_payload(shm_key, vecs, tids, n_vecs, dim);
    if (shm_fd < 0) {
        LOG_ERROR("[cuvs_ipc_build_ivfpq] shm_write FAILED errno=%d (%s)\n",
                  errno, strerror(errno));
        goto cleanup;
    }

    sock = uds_connect_ex(socket_path, 600);  /* IVF-PQ build can take minutes */
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_build_ivfpq] uds_connect FAILED errno=%d (%s)\n",
                  errno, strerror(errno));
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.op          = CUVS_OP_BUILD_IVFPQ;
    cmd.db_oid      = db_oid;
    cmd.index_oid   = index_oid;
    cmd.metric      = metric;
    cmd.dim         = (uint32_t)dim;
    cmd.n_vecs      = n_vecs;
    cmd.table_oid   = table_oid;
    cmd.relfilenode = relfilenode;
    cmd.n_lists     = n_lists;
    cmd.pq_bits     = pq_bits;
    cmd.pq_dim      = pq_dim;
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, -1, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0) close(sock);
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_unlink(shm_key);
    }
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search_ivfpq (3P)
 *
 * Sends a SEARCH_IVFPQ request. Query is written to shm; results arrive
 * inline in the reply (same CuvsResult array as regular SEARCH).
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search_ivfpq(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *query_vec,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      n_probes,
    uint64_t     *tids_out,
    float        *dist_out,
    int          *n_out,
    uint32_t     *latency_us_out)
{
    char shm_key[64] = "";   /* stays empty if make_shm_key fails */
    int  shm_fd = -1;
    int  sock   = -1;
    int  rc     = CUVS_STATUS_ERROR;

    if (latency_us_out) *latency_us_out = 0;
    if (n_out)          *n_out = 0;

    if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
        goto cleanup;
    shm_fd = shm_write_query(shm_key, query_vec, dim);
    if (shm_fd < 0)
        goto cleanup;

    sock = uds_connect(socket_path);
    if (sock < 0) {
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_SEARCH_IVFPQ,
        .db_oid    = db_oid,
        .index_oid = index_oid,
        .k         = (uint32_t)k,
        .metric    = metric,
        .dim       = (uint32_t)dim,
        .n_probes  = n_probes,
    };
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    {
        int wr = recv_reply_interruptible(sock, &hdr, 30);
        if (wr == 1) { rc = CUVS_STATUS_CANCELED; goto cleanup; }
        if (wr < 0)  goto cleanup;
    }

    rc = (int)hdr.status;
    if (latency_us_out)
        *latency_us_out = hdr.latency_us;

    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n = (int)hdr.n_results;
        int n_write = (n > k) ? k : n;
        CuvsResult *results = malloc(n * sizeof(CuvsResult));
        if (!results) { rc = CUVS_STATUS_ERROR; goto cleanup; }

        if (recv_all(sock, results, n * sizeof(CuvsResult)) < 0)
        {
            free(results);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        for (int i = 0; i < n_write; i++)
        {
            tids_out[i] = results[i].tid;
            dist_out[i] = results[i].distance;
        }
        *n_out = n_write;
        free(results);
    }
    else
    {
        *n_out = 0;
    }

cleanup:
    if (sock >= 0)   close(sock);
    if (shm_fd >= 0) close(shm_fd);
    if (shm_key[0])
        shm_unlink(shm_key);
    return rc;
}

/* ----------------------------------------------------------------
 * 3Q: cuvs_ipc_extend
 *
 * Writes n_new vectors + their TIDs to a named shm segment (same layout as
 * BUILD: [float32 vecs][uint64_t tids]) then sends CUVS_OP_EXTEND. The daemon
 * calls cagra::extend in-place on the loaded CAGRA index. Fast path: timeout
 * is 30 s (extend is GPU-bound, not O(index_size) on disk).
 * ---------------------------------------------------------------- */
int
cuvs_ipc_extend(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    const float    *new_vecs,
    const uint64_t *new_tids,
    int64_t         n_new,
    int             dim,
    uint32_t        max_chunk_size,
    const char     *index_dir)
{
    char shm_key[64] = "";
    int  shm_fd  = -1;
    int  sock    = -1;
    int  rc      = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    char            dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    if (make_shm_key(shm_key, sizeof(shm_key)) != 0)   /* #87 finding 2 */
        goto cleanup;
    shm_fd = shm_write_build_payload(shm_key, new_vecs, new_tids, n_new, dim);
    if (shm_fd < 0) {
        LOG_ERROR("[cuvs_ipc_extend] shm_write FAILED errno=%d (%s)\n",
                  errno, strerror(errno));
        goto cleanup;
    }

    sock = uds_connect_ex(socket_path, 30);
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_extend] uds_connect FAILED errno=%d (%s)\n",
                  errno, strerror(errno));
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.op             = CUVS_OP_EXTEND;
    cmd.db_oid         = db_oid;
    cmd.index_oid      = index_oid;
    cmd.dim            = (uint32_t)dim;
    cmd.n_vecs         = n_new;
    cmd.max_chunk_size = max_chunk_size;
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, -1, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0)   close(sock);
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_unlink(shm_key);
    }
    return rc;
}

/* ----------------------------------------------------------------
 * 3Q: cuvs_ipc_compact
 *
 * Sends CUVS_OP_COMPACT with no extra payload — the daemon reads the
 * .tombstone sidecar directly from index_dir. Timeout is 120 s because
 * cagra::merge rebuilds the graph (O(new_n * graph_degree)).
 * ---------------------------------------------------------------- */
int
cuvs_ipc_compact(
    const char *socket_path,
    uint32_t    db_oid,
    uint32_t    index_oid,
    const char *index_dir)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    char            dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op        = CUVS_OP_COMPACT;
    cmd.db_oid    = db_oid;
    cmd.index_oid = index_oid;

    if (send_cmd(sock, &cmd) < 0)
        goto cleanup;

    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, -1, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    if (recv_reply(sock, &hdr) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0) close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * cuvs_ipc_set_vram_budget — override per-GPU VRAM budget at runtime.
 *
 * budget_bytes == 0 restores the unlimited default. No payload beyond
 * the cmd frame (n_vecs carries the value). Used by tests and admin.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_set_vram_budget(const char *socket_path, int64_t budget_bytes)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    CuvsReplyHeader hdr;

    sock = uds_connect_ex(socket_path, 10);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op     = CUVS_OP_SET_VRAM_BUDGET;
    cmd.n_vecs = budget_bytes;

    if (send_cmd(sock, &cmd) >= 0 &&
        recv_reply(sock, &hdr) >= 0)
        rc = (int)hdr.status;

    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * cuvs_ipc_eat_vram / cuvs_ipc_free_vram — test helpers.
 *
 * EAT: tell the daemon to cudaMalloc (free_vram - leave_bytes) on
 * device_id, leaving only leave_bytes of VRAM free.  Subsequent GPU
 * operations that need more than that hit physical CUDA OOM instead
 * of the budget-check path.  FREE: release that allocation.
 *
 * leave_bytes in n_vecs; device_id in dim.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_eat_vram(const char *socket_path, int64_t leave_bytes, int device_id)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    CuvsReplyHeader hdr;

    sock = uds_connect_ex(socket_path, 10);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op     = CUVS_OP_EAT_VRAM;
    cmd.n_vecs = leave_bytes;
    cmd.dim    = (uint32_t)device_id;

    if (send_cmd(sock, &cmd) >= 0 &&
        recv_reply(sock, &hdr) >= 0)
        rc = (int)hdr.status;

    close(sock);
    return rc;
}

int
cuvs_ipc_free_vram(const char *socket_path, int device_id)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    CuvsReplyHeader hdr;

    sock = uds_connect_ex(socket_path, 10);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op  = CUVS_OP_FREE_VRAM;
    cmd.dim = (uint32_t)device_id;

    if (send_cmd(sock, &cmd) >= 0 &&
        recv_reply(sock, &hdr) >= 0)
        rc = (int)hdr.status;

    close(sock);
    return rc;
}

int
cuvs_ipc_inject_extend_oom(const char *socket_path, int enable)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    CuvsReplyHeader hdr;

    sock = uds_connect_ex(socket_path, 10);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op  = CUVS_OP_INJECT_EXTEND_OOM;
    cmd.dim = (uint32_t)(enable ? 1 : 0);

    if (send_cmd(sock, &cmd) >= 0 &&
        recv_reply(sock, &hdr) >= 0)
        rc = (int)hdr.status;

    close(sock);
    return rc;
}

int
cuvs_ipc_inject_build_oom(const char *socket_path, int n_fail)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame    cmd;
    CuvsReplyHeader hdr;

    sock = uds_connect_ex(socket_path, 10);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op  = CUVS_OP_INJECT_BUILD_OOM;
    cmd.dim = (uint32_t)(n_fail < 0 ? 0 : n_fail);

    if (send_cmd(sock, &cmd) >= 0 &&
        recv_reply(sock, &hdr) >= 0)
        rc = (int)hdr.status;

    close(sock);
    return rc;
}

/* Temporary HNSW jobs use ordinary disk and a separate owner lease. Cancel
 * never frees GPU state asynchronously: the daemon observes the lease at safe
 * boundaries and destroys cuVS objects after any in-flight call returns. */
static int g_hnsw_backend_cancel = 0;

int
cuvs_ipc_hnsw_backend_cancelled(void)
{
    return g_hnsw_backend_cancel;
}

int
cuvs_ipc_build_hnsw_file(const char *socket_path, int vectors_fd,
    uint64_t n_vecs, uint32_t dim, uint32_t metric, uint32_t graph_degree,
    uint32_t intermediate_degree, uint32_t hierarchy, uint64_t host_budget,
    uint32_t build_algo, uint32_t ef_construction, CuvsHnswPolicy *policy,
    CuvsHnswBudgetSnapshot *budget, CuvsHnswAceReadback *ace_readback,
    CuvsHnswNnReadback *nn_readback,
    int *hnsw_fd, char *error, size_t error_len)
{
    return cuvs_ipc_build_hnsw_file_idle(socket_path, vectors_fd, n_vecs, dim,
        metric, graph_degree, intermediate_degree, hierarchy, host_budget,
        build_algo, ef_construction, policy, budget, ace_readback, nn_readback, hnsw_fd,
        error, error_len, NULL, NULL);
}

/* Same request/reply as above. While idle work remains, the wait polls the
 * socket without blocking and runs one unit of work per empty poll; the
 * reply is read as soon as it is ready, even if work remains. Idle
 * iterations do not count toward the 864000 x 100 ms wait limit. */
int
cuvs_ipc_build_hnsw_file_idle(const char *socket_path, int vectors_fd,
    uint64_t n_vecs, uint32_t dim, uint32_t metric, uint32_t graph_degree,
    uint32_t intermediate_degree, uint32_t hierarchy, uint64_t host_budget,
    uint32_t build_algo, uint32_t ef_construction, CuvsHnswPolicy *policy,
    CuvsHnswBudgetSnapshot *budget, CuvsHnswAceReadback *ace_readback,
    CuvsHnswNnReadback *nn_readback,
    int *hnsw_fd, char *error, size_t error_len,
    CuvsIpcIdleWork idle, void *idle_arg)
{
    int idle_more = idle != NULL;
    int sock = -1, owner[2] = {-1, -1}, result_fd = -1;
    int rc = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd = {0};
    CuvsHnswFileRequest request = {CUVS_HNSW_FILE_VERSION, hierarchy, host_budget, build_algo, ef_construction};
    CuvsHnswFileReply wire = {0};
    CuvsReplyHeader reply = {0};
    char token = 'O';
    g_hnsw_backend_cancel = 0;
    forget_reply();
    *hnsw_fd = -1;
    if (policy) memset(policy, 0, sizeof(*policy));
    if (ace_readback) memset(ace_readback, 0, sizeof(*ace_readback));
    if (nn_readback) memset(nn_readback, 0, sizeof(*nn_readback));
    if (budget) memset(budget, 0, sizeof(*budget));
    if (error_len) error[0] = '\0';
    if (!cuvs_hnsw_regular_disk(vectors_fd) || !n_vecs || n_vecs > INT32_MAX
        || !dim || metric > CUVS_METRIC_IP || !cuvs_hnsw_file_request_supported(&request))
        goto done;
    sock = uds_connect_ex(socket_path, 30);
    if (sock < 0) { rc = CUVS_STATUS_UNAVAILABLE; goto done; }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, owner)) goto done;
    fcntl(owner[0], F_SETFD, FD_CLOEXEC);
    fcntl(owner[1], F_SETFD, FD_CLOEXEC);
    cmd.op = CUVS_OP_BUILD_HNSW_FILE;
    cmd.n_vecs = (int64_t)n_vecs;
    cmd.dim = dim;
    cmd.metric = metric;
    cmd.graph_degree = graph_degree;
    cmd.intermediate_graph_degree = intermediate_degree;
    if (send_cmd(sock, &cmd)
        || cuvs_fd_send(sock, vectors_fd, &request, sizeof(request))
        || cuvs_fd_send(sock, owner[1], &token, 1)) goto done;
    /* Keep our copy of owner[1] until done: on macOS, closing it while the
     * SCM_RIGHTS message is still in flight lets the daemon's received copy
     * read EOF, i.e. a spurious cancel. Cancel still works because
     * EOF on the daemon's end follows from closing owner[0], its peer. */
    /* A single readiness byte permits interruptible waiting before recvmsg:
     * reading that byte cannot accidentally discard the ancillary result FD. */
    for (unsigned waited = 0; waited < 864000; )
    {
        struct pollfd pfd = {.fd = sock, .events = POLLIN};
        int pr;
        if (g_wait_cb && g_wait_cb())
        {
            struct stat input;
            memset(&input, 0, sizeof(input));
            (void)fstat(vectors_fd, &input);
            LOG_INFO("[hnsw_wait] phase=backend-cancel mono_us=%llu input_dev=%llu input_ino=%llu\n",
                (unsigned long long)cuvs_hnsw_monotonic_us(),
                (unsigned long long)input.st_dev, (unsigned long long)input.st_ino);
            rc = CUVS_STATUS_CANCELED;
            g_hnsw_backend_cancel = 1;
            token = 'C';
            (void)send(owner[0], &token, 1, MSG_DONTWAIT);
            goto done;
        }
        pr = poll(&pfd, 1, idle_more ? 0 : 100);
        if (pr == 0 && idle_more)
        {
            idle_more = idle(idle_arg);
            continue;
        }
        waited++;
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) goto done;
        if (pr == 0) continue;
        if (recv_all(sock, &token, 1) || token != 'R'
            || cuvs_fd_recv(sock, &wire, sizeof(wire), &result_fd))
        {
            if (error_len) snprintf(error, error_len,
                "incomplete HNSW build reply; check connection and co-deploy matching extension and daemon");
            goto done;
        }
        reply = wire.header;
        remember_reply(&reply);
        rc = (int)reply.status;
        if (error_len) snprintf(error, error_len, "%.*s", (int)sizeof(reply.error), reply.error);
        /* Decision bytes for the status-13 DETAIL; present on refusals too. */
        if (budget) *budget = wire.budget;
        if (rc == CUVS_STATUS_OK)
        {
            if (!cuvs_hnsw_policy_valid(&wire.policy)
                || wire.policy.requested_algo != build_algo
                || wire.policy.requested_ef != ef_construction
                || wire.policy.n_rows != n_vecs || wire.policy.dim != dim
                || wire.policy.metric != metric || wire.policy.hierarchy != hierarchy
                || (wire.policy.route == CUVS_HNSW_ROUTE_ACE
                    ? !cuvs_hnsw_ace_readback_valid(&wire.ace, &wire.policy)
                    : wire.ace.magic != 0)
                || (wire.policy.route == CUVS_HNSW_ROUTE_NN_DESCENT
                    ? !cuvs_hnsw_nn_readback_valid(&wire.nn, &wire.policy)
                    : wire.nn.magic != 0)
                || !cuvs_hnsw_regular_disk(result_fd)
                || (fcntl(result_fd, F_GETFL) & O_ACCMODE) != O_RDONLY)
            { rc = CUVS_STATUS_ERROR; goto done; }
            fcntl(result_fd, F_SETFD, FD_CLOEXEC);
            token = 'A';
            if (send(owner[0], &token, 1, 0) != 1) { rc = CUVS_STATUS_ERROR; goto done; }
            if (policy) *policy = wire.policy;
            if (ace_readback) *ace_readback = wire.ace;
            if (nn_readback) *nn_readback = wire.nn;
            *hnsw_fd = result_fd; result_fd = -1;
        }
        goto done;
    }
    if (error_len) snprintf(error, error_len, "temporary HNSW job timed out");
 done:
    if (result_fd >= 0) close(result_fd);
    if (owner[0] >= 0) close(owner[0]);
    if (owner[1] >= 0) close(owner[1]);
    if (sock >= 0) close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * I3-B: manual cache ops, reclaim parameter change, text status ops.
 * ---------------------------------------------------------------- */
static int
ipc_header_only(const char *socket_path, CuvsCmdFrame *cmd, char *msg,
                size_t msglen)
{
    CuvsReplyHeader hdr;
    int rc = CUVS_STATUS_ERROR;
    int sock;
    forget_reply();
    sock = uds_connect_ex(socket_path, 30);

    if (msg && msglen)
        msg[0] = '\0';
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;
    if (send_cmd(sock, cmd) == 0 && recv_all(sock, &hdr, sizeof(hdr)) == 0)
    {
        remember_reply(&hdr);
        rc = (int) hdr.status;
        if (msg && msglen)
        {
            hdr.error[sizeof(hdr.error) - 1] = '\0';
            snprintf(msg, msglen, "%s", hdr.error);
        }
    }
    close(sock);
    return rc;
}

int
cuvs_ipc_manual(const char *socket_path, uint32_t op, uint32_t db_oid,
                uint32_t index_oid, char *msg, size_t msglen)
{
    CuvsCmdFrame cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = op;
    cmd.db_oid = db_oid;
    cmd.index_oid = index_oid;
    return ipc_header_only(socket_path, &cmd, msg, msglen);
}

int
cuvs_ipc_reclaim_set(const char *socket_path, const char *name,
                     const char *value, char *msg, size_t msglen)
{
    CuvsCmdFrame cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = CUVS_OP_RECLAIM_SET;
    if (strlen(name) >= sizeof(cmd.shm_key) ||
        strlen(value) >= sizeof(cmd.filter_shm_key))
    {
        if (msg && msglen)
            snprintf(msg, msglen, "parameter name or value too long");
        return CUVS_STATUS_ERROR;
    }
    snprintf(cmd.shm_key, sizeof(cmd.shm_key), "%s", name);
    snprintf(cmd.filter_shm_key, sizeof(cmd.filter_shm_key), "%s", value);
    return ipc_header_only(socket_path, &cmd, msg, msglen);
}

int
cuvs_ipc_text(const char *socket_path, uint32_t op, char **text, size_t *len)
{
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr;
    int rc = CUVS_STATUS_ERROR;
    int sock = uds_connect_ex(socket_path, 30);

    *text = NULL;
    if (len)
        *len = 0;
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = op;
    if (send_cmd(sock, &cmd) == 0 && recv_all(sock, &hdr, sizeof(hdr)) == 0)
    {
        rc = (int) hdr.status;
        if (rc == CUVS_STATUS_OK)
        {
            char *buf;
            if (hdr.n_results > CUVS_IPC_TEXT_MAX ||
                (buf = malloc((size_t) hdr.n_results + 1)) == NULL)
                rc = CUVS_STATUS_ERROR;
            else if (hdr.n_results > 0 &&
                     recv_all(sock, buf, hdr.n_results) < 0)
            {
                free(buf);
                rc = CUVS_STATUS_ERROR;
            }
            else
            {
                buf[hdr.n_results] = '\0';
                *text = buf;
                if (len)
                    *len = hdr.n_results;
            }
        }
    }
    close(sock);
    return rc;
}

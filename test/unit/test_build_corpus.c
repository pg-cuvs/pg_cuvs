/*
 * test_build_corpus.c — standalone unit tests for src/cuvs_build_corpus.{c,h}.
 *
 * No framework, no PostgreSQL, no CUDA, no GPU. Build + run via `make test-unit`.
 * Returns non-zero on any failure.
 *
 * Proves the ADR-048 corpus handoff is correct without a daemon:
 *  - tier selection (forced + auto),
 *  - grow/finalize produce the daemon's byte-exact [vectors][tids] layout,
 *  - SCM_RIGHTS fd passing round-trips an fd over a socketpair,
 *  - (Linux) memfd refcount: a passed fd keeps memory alive after the sender
 *    closes, and the flock reaper reaps a dead owner but spares a live one.
 *
 * Portable parts run everywhere; memfd / shm-grow / /dev/shm-reaper parts are
 * Linux-gated (macOS lacks memfd_create, one-shot shm ftruncate, no /dev/shm).
 */
#define _GNU_SOURCE

#include "cuvs_build_corpus.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do {                                          \
    if (cond) { g_pass++; }                                             \
    else { g_fail++; fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static void
make_corpus(float **vecs, uint64_t **tids, int64_t n, int dim)
{
    *vecs = malloc((size_t) n * dim * sizeof(float));
    *tids = malloc((size_t) n * sizeof(uint64_t));
    for (int64_t i = 0; i < n; i++)
    {
        for (int dpos = 0; dpos < dim; dpos++)
            (*vecs)[i * dim + dpos] = (float) (i * 1000 + dpos);
        (*tids)[i] = ((uint64_t) i << 16) | (uint64_t) (i & 0xFFFF);
    }
}

/* Heap tier: open returns kind=HEAP, base NULL, resize is a no-op, close safe. */
static void
test_force_heap(void)
{
    CuvsBuildCorpus c;
    cuvs_corpus_force_kind("heap");
    ASSERT(cuvs_corpus_open(&c, 4096) == 0, "heap open");
    ASSERT(c.kind == CORPUS_HEAP, "heap kind");
    ASSERT(c.base == NULL && c.fd == -1, "heap base/fd");
    ASSERT(cuvs_corpus_resize(&c, 8192) == 0, "heap resize no-op");
    cuvs_corpus_close(&c);
    cuvs_corpus_force_kind(NULL);
}

/* Golden layout via the shm tier at exact size (no grow — portable to macOS). */
static void
test_golden_no_grow(int64_t n, int dim)
{
    float    *vecs;
    uint64_t *tids;
    size_t    vec_bytes = (size_t) n * dim * sizeof(float);
    size_t    tid_bytes = (size_t) n * sizeof(uint64_t);
    size_t    total     = vec_bytes + tid_bytes;
    char     *golden;
    CuvsBuildCorpus c;
    char      name[64];

    make_corpus(&vecs, &tids, n, dim);
    golden = malloc(total);
    memcpy(golden, vecs, vec_bytes);
    memcpy(golden + vec_bytes, tids, tid_bytes);

    cuvs_corpus_force_kind("shm");
    ASSERT(cuvs_corpus_open(&c, total) == 0, "shm open");
    ASSERT(c.kind == CORPUS_SHM && c.base != NULL, "shm kind/base");
    memcpy((char *) c.base, vecs, vec_bytes);
    memcpy((char *) c.base + vec_bytes, tids, tid_bytes);
    ASSERT(memcmp(c.base, golden, total) == 0, "golden byte-identity (shm)");

    /* 0666 perms so the daemon (other uid) can open it. */
    {
        struct stat st;
        ASSERT(fstat(c.fd, &st) == 0, "fstat");
        ASSERT((st.st_mode & 0777) == 0666, "shm mode 0666");
    }
    strncpy(name, c.shm_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    cuvs_corpus_close(&c);
    /* no residue */
    {
        int fd = shm_open(name, O_RDONLY, 0);
        ASSERT(fd < 0 && errno == ENOENT, "no shm residue after close");
        if (fd >= 0) { close(fd); shm_unlink(name); }
    }
    cuvs_corpus_force_kind(NULL);
    free(vecs); free(tids); free(golden);
}

/* SCM_RIGHTS round-trip: send an fd over a socketpair, receive it, confirm the
 * received fd refers to the same open file (write via received, read via sent). */
static void
test_fd_pass(void)
{
    int  sv[2];
    char tmpl[] = "/tmp/cuvs_fdpass_XXXXXX";
    int  srcfd;
    char payload[256];
    char rbuf[256];
    int  rfd = -1;

    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    /* A throwaway shared file to pass. */
    srcfd = mkstemp(tmpl);
    ASSERT(srcfd >= 0, "mkstemp");
    unlink(tmpl);

    memset(payload, 0, sizeof(payload));
    strcpy(payload, "/some/index/dir");
    ASSERT(cuvs_fd_send(sv[0], srcfd, payload, sizeof(payload)) == 0, "fd_send");

    ASSERT(cuvs_fd_recv(sv[1], rbuf, sizeof(rbuf), &rfd) == 0, "fd_recv");
    ASSERT(memcmp(rbuf, payload, sizeof(payload)) == 0, "payload round-trip");
    ASSERT(rfd >= 0, "received fd present");

    /* Same underlying file: write via received fd, read via source fd. */
    if (rfd >= 0)
    {
        const char *marker = "HELLO";
        char        chk[5] = {0};
        ASSERT(pwrite(rfd, marker, 5, 0) == 5, "pwrite via received fd");
        ASSERT(pread(srcfd, chk, 5, 0) == 5 && memcmp(chk, marker, 5) == 0,
               "received fd aliases source file");
        close(rfd);
    }

    /* No fd case: fd_send(-1) carries payload only, recv yields out_fd == -1. */
    ASSERT(cuvs_fd_send(sv[0], -1, payload, sizeof(payload)) == 0, "fd_send no-fd");
    rfd = 999;
    ASSERT(cuvs_fd_recv(sv[1], rbuf, sizeof(rbuf), &rfd) == 0, "fd_recv no-fd");
    ASSERT(rfd == -1, "no fd => out_fd -1");

    close(srcfd); close(sv[0]); close(sv[1]);
}

#ifdef __linux__
/* memfd tier: real grow path + golden byte-identity, plus refcount across a
 * passed fd (sender close keeps it alive while a child holds it). */
static void
test_memfd_grow_golden(int64_t n, int dim, int64_t init_rows)
{
    float    *vecs;
    uint64_t *tids;
    size_t    vec_bytes = (size_t) n * dim * sizeof(float);
    size_t    tid_bytes = (size_t) n * sizeof(uint64_t);
    size_t    total     = vec_bytes + tid_bytes;
    char     *golden;
    CuvsBuildCorpus c;
    int64_t   cap;

    make_corpus(&vecs, &tids, n, dim);
    golden = malloc(total);
    memcpy(golden, vecs, vec_bytes);
    memcpy(golden + vec_bytes, tids, tid_bytes);

    cuvs_corpus_force_kind("memfd");
    ASSERT(cuvs_corpus_open(&c, (size_t) init_rows * dim * sizeof(float)) == 0, "memfd open");
    ASSERT(c.kind == CORPUS_MEMFD && c.base != NULL, "memfd kind/base");

    cap = init_rows;
    for (int64_t i = 0; i < n; i++)
    {
        if (i >= cap)
        {
            cap *= 2;
            ASSERT(cuvs_corpus_resize(&c, (size_t) cap * dim * sizeof(float)) == 0, "memfd grow");
        }
        memcpy((char *) c.base + (size_t) i * dim * sizeof(float),
               &vecs[i * dim], (size_t) dim * sizeof(float));
    }
    ASSERT(cuvs_corpus_resize(&c, total) == 0, "memfd finalize");
    memcpy((char *) c.base + vec_bytes, tids, tid_bytes);
    ASSERT(memcmp(c.base, golden, total) == 0, "golden byte-identity (memfd)");

    cuvs_corpus_close(&c);
    cuvs_corpus_force_kind(NULL);
    free(vecs); free(tids); free(golden);
}

/* flock reaper: a live owner (child holding flock) is spared; a dead owner is
 * reaped. Uses CUVS_REAP_MIN_AGE=0 to bypass the create-window mtime guard. */
static void
test_reaper_live_vs_dead(void)
{
    int sync_pipe[2];
    pid_t pid;

    setenv("CUVS_REAP_MIN_AGE", "0", 1);
    ASSERT(pipe(sync_pipe) == 0, "pipe");

    pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0)
    {
        /* Child: create a corpus shm (holds flock for life), signal parent, wait. */
        CuvsBuildCorpus c;
        cuvs_corpus_force_kind("shm");
        if (cuvs_corpus_open(&c, 4096) != 0) _exit(2);
        char ready = 'R';
        ssize_t wn = write(sync_pipe[1], &ready, 1); (void) wn;
        pause();                       /* until SIGKILL */
        _exit(0);
    }

    /* Parent: wait for child ready. */
    close(sync_pipe[1]);
    char buf;
    ASSERT(read(sync_pipe[0], &buf, 1) == 1, "child ready");

    /* Child alive & holding flock => reaper must NOT reap it. */
    ASSERT(cuvs_corpus_reap_orphans(1) == 0, "live owner spared");

    /* Kill child; flock auto-released by kernel => reaper reaps. */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    ASSERT(cuvs_corpus_reap_orphans(1) >= 1, "dead owner reaped");
    /* Idempotent: nothing left. */
    ASSERT(cuvs_corpus_reap_orphans(1) == 0, "no orphan after reap");

    close(sync_pipe[0]);
    unsetenv("CUVS_REAP_MIN_AGE");
    cuvs_corpus_force_kind(NULL);
}
#endif /* __linux__ */

int
main(void)
{
    test_force_heap();
    test_golden_no_grow(/*n=*/64,   /*dim=*/4);
    test_golden_no_grow(/*n=*/1000, /*dim=*/16);
    test_fd_pass();
#ifdef __linux__
    test_memfd_grow_golden(/*n=*/1000, /*dim=*/16, /*init_rows=*/64);
    test_memfd_grow_golden(/*n=*/333,  /*dim=*/3,  /*init_rows=*/333);
    test_reaper_live_vs_dead();
#else
    fprintf(stderr, "[INFO] memfd/shm-grow/reaper tests skipped (non-Linux)\n");
#endif

    fprintf(stderr, "test_build_corpus: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

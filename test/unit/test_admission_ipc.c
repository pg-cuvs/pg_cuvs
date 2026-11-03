/*
 * CPU wire test for the I3 reply fields (v6 r18 C5, ADR-081 version 5):
 * the real cuvs_ipc.c client against a one-shot mock daemon on a UDS.
 * Checks that err_out / notice_out ride every reply header, are exposed by
 * cuvs_ipc_last_err_out() / cuvs_ipc_last_notice_out(), and are cleared
 * before the next request so a stale rejection is never re-reported.
 */
#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "cuvs_ipc.h"
#include "cuvs_admission_contract.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int g_failures;
#define CHECK(label, cond)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s (line %d)\n", (label), __LINE__);      \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

_Static_assert(CUVS_PROTO_VERSION == CUVS_ADMISSION_IPC_VERSION,
               "I3 wire version is bound to the shared admission contract");
_Static_assert(sizeof(((CuvsReplyHeader *)0)->err_out) == CUVS_ADMISSION_ERR_OUT_BYTES,
               "err_out is the 512 B payload");
_Static_assert(sizeof(((CuvsReplyHeader *)0)->notice_out) == CUVS_NOTICE_OUT_BYTES,
               "notice_out size");

typedef struct Mock {
    int listener;
    uint32_t seen_version;
    CuvsReplyHeader reply;
} Mock;

static void *
mock_daemon(void *arg)
{
    Mock *m = (Mock *)arg;
    CuvsCmdFrame cmd;
    int c = accept(m->listener, NULL, NULL);
    if (c < 0)
        return NULL;
    if (recv(c, &cmd, sizeof(cmd), MSG_WAITALL) == (ssize_t)sizeof(cmd))
        m->seen_version = cmd.proto_version;
    (void)!write(c, &m->reply, sizeof(m->reply));
    close(c);
    return NULL;
}

static int
round_trip_op(const char *path, const CuvsReplyHeader *reply, uint32_t *version, int manual)
{
    Mock m;
    struct sockaddr_un a;
    pthread_t t;
    int rc;

    memset(&m, 0, sizeof(m));
    m.reply = *reply;
    unlink(path);
    m.listener = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (m.listener < 0 || bind(m.listener, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(m.listener, 1) != 0)
        return -100;
    pthread_create(&t, NULL, mock_daemon, &m);
    char msg[256];
    rc = manual ? cuvs_ipc_manual(path, CUVS_OP_LOAD, 1, 2, msg, sizeof(msg))
                : cuvs_ipc_drop(path, 1, 2);
    pthread_join(t, NULL);
    close(m.listener);
    unlink(path);
    if (version)
        *version = m.seen_version;
    return rc;
}

static int
round_trip(const char *path, const CuvsReplyHeader *reply, uint32_t *version)
{
    return round_trip_op(path, reply, version, 0);
}

int
main(void)
{
    char path[96];
    CuvsReplyHeader r;
    uint32_t version = 0;
    int rc;

    snprintf(path, sizeof(path), "/tmp/pgcuvs-adm-%d.sock", (int)getpid());

    /* A rejection reply carries the payload and a notice. */
    memset(&r, 0, sizeof(r));
    r.status = CUVS_STATUS_OOM_FALLBACK;
    snprintf(r.error, sizeof(r.error), "admission rejected");
    snprintf(r.err_out, sizeof(r.err_out), "AR0 n=27895 q=23847");
    snprintf(r.notice_out, sizeof(r.notice_out), "pg_cuvs: registry full; fallback");
    rc = round_trip(path, &r, &version);
    CHECK("status", rc == CUVS_STATUS_OOM_FALLBACK);
    CHECK("client sends version 5", version == 5u);
    CHECK("err_out exposed", strcmp(cuvs_ipc_last_err_out(), "AR0 n=27895 q=23847") == 0);
    CHECK("notice exposed",
          strcmp(cuvs_ipc_last_notice_out(), "pg_cuvs: registry full; fallback") == 0);

    /* B manual LOAD consumes the same err_out / notice_out path. */
    rc = round_trip_op(path, &r, NULL, 1);
    CHECK("manual rejection status", rc == CUVS_STATUS_OOM_FALLBACK);
    CHECK("manual err_out exposed", strcmp(cuvs_ipc_last_err_out(), "AR0 n=27895 q=23847") == 0);
    CHECK("manual notice exposed", strcmp(cuvs_ipc_last_notice_out(),
          "pg_cuvs: registry full; fallback") == 0);
    {
        CuvsReplyHeader ok = {0};
        CHECK("manual clean success", round_trip_op(path, &ok, NULL, 1) == CUVS_STATUS_OK);
        CHECK("manual stale rejection cleared", cuvs_ipc_last_err_out()[0] == '\0');
        CHECK("manual stale notice cleared", cuvs_ipc_last_notice_out()[0] == '\0');
    }

    /* An unterminated field is cut at its own size, never read beyond it. */
    memset(&r, 'x', sizeof(r));
    r.status = CUVS_STATUS_OOM_FALLBACK;
    rc = round_trip(path, &r, NULL);
    CHECK("unterminated status", rc == CUVS_STATUS_OOM_FALLBACK);
    CHECK("err_out bounded",
          strlen(cuvs_ipc_last_err_out()) == CUVS_ADMISSION_ERR_OUT_BYTES - 1);
    CHECK("notice bounded", strlen(cuvs_ipc_last_notice_out()) == CUVS_NOTICE_OUT_BYTES - 1);

    /* The next request starts clean: an OK reply leaves both empty. */
    memset(&r, 0, sizeof(r));
    r.status = CUVS_STATUS_OK;
    rc = round_trip(path, &r, NULL);
    CHECK("ok status", rc == CUVS_STATUS_OK);
    CHECK("err_out cleared", cuvs_ipc_last_err_out()[0] == '\0');
    CHECK("notice cleared", cuvs_ipc_last_notice_out()[0] == '\0');

    /* A daemon that never answers leaves nothing stale either. */
    snprintf(r.err_out, sizeof(r.err_out), "AR3 n=1");
    (void)round_trip(path, &r, NULL);
    CHECK("primed", cuvs_ipc_last_err_out()[0] == 'A');
    rc = cuvs_ipc_drop("/tmp/pgcuvs-adm-missing.sock", 1, 2);
    CHECK("unavailable", rc == CUVS_STATUS_UNAVAILABLE);
    CHECK("cleared on connect failure", cuvs_ipc_last_err_out()[0] == '\0');

    if (g_failures) {
        fprintf(stderr, "test_admission_ipc: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_admission_ipc: all checks passed (wire v%u)\n", CUVS_PROTO_VERSION);
    return 0;
}

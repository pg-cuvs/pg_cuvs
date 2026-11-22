/* the daemon answers a request only with a build made for the index relfilenode the request carries, and an export only with
 * one made for the heap relfilenode too (entry_current_as). The planner's record check normally keeps such requests from reaching the daemon, and no SQL sequence was found
 * that gets past it (build_identity_cross.spec), so the daemon's own check is proven here, on the real handlers with the CPU shim: a
 * request with the entry's values gets past the check (the metric check that follows it answers METRIC_MISMATCH), one with another
 * value or with 0 is refused NOT_CURRENT. A build loaded with a record of an earlier version knows only the heap relfilenode and answers a
 * request for that heap (nothing is written to accept it). And read_index_identity on each record format. */
#define _GNU_SOURCE
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static int failures;
#define CHECK(what, cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s (line %d)\n", what, __LINE__); failures++; } } while (0)

static uint32_t reply(void (*handler)(int, const CuvsCmdFrame *), const CuvsCmdFrame *cmd)
{
    int sv[2];
    CuvsReplyHeader hdr;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    handler(sv[1], cmd);
    assert(recv(sv[0], &hdr, sizeof(hdr), MSG_WAITALL) == (ssize_t) sizeof(hdr));
    close(sv[0]); close(sv[1]);
    return hdr.status;
}

/* The same in a child process: a handler that gets past the identity check goes on to the test's fake graph handle and crashes, which
 * here is a reply that never comes (UINT32_MAX) and a FAIL line instead of a crash of the whole test. For requests whose handler does
 * not change the registry (the child's changes are lost). */
static uint32_t reply_isolated(void (*handler)(int, const CuvsCmdFrame *), const CuvsCmdFrame *cmd)
{
    int sv[2], st;
    CuvsReplyHeader hdr;
    pid_t pid;
    uint32_t out = UINT32_MAX;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0)
    {
        close(sv[0]);
        handler(sv[1], cmd);
        _exit(0);
    }
    close(sv[1]);
    if (recv(sv[0], &hdr, sizeof(hdr), MSG_WAITALL) == (ssize_t) sizeof(hdr))
        out = hdr.status;
    close(sv[0]);
    (void) waitpid(pid, &st, 0);
    return out;
}

int main(void)
{
    char dir[] = "/tmp/m16-identity-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(g_index_dir, sizeof(g_index_dir), "%s", dir);

    /* One resident cagra entry (1/2) and one ivfpq entry (1/3), built for index relfilenodes 5002 and 5003 of heap 16384. The handles
     * are never used: every request below stops at the identity or the metric check. */
    g_indexes = calloc(2, sizeof(IndexEntry));
    g_n_indexes = 2;
    IndexEntry *c = &g_indexes[0], *q = &g_indexes[1];
    c->valid = 1; c->db_oid = 1; c->index_oid = 2; c->dim = 4; c->metric = CUVS_METRIC_L2; c->handle = (CuvsCagraIndex) (uintptr_t) 1;
    c->index_rfn = 5002; c->heap_rfn = 16384;
    q->valid = 1; q->db_oid = 1; q->index_oid = 3; q->dim = 4; q->metric = CUVS_METRIC_L2; q->ivfpq_handle = (void *) (uintptr_t) 1;
    q->index_rfn = 5003; q->heap_rfn = 16384;

    CuvsCmdFrame s = {.op = CUVS_OP_SEARCH, .db_oid = 1, .index_oid = 2, .dim = 4, .k = 1, .metric = CUVS_METRIC_IP};
    s.index_relfilenode = 5002; s.heap_relfilenode = 16384;
    CHECK("cagra search with the build's identity gets past the check", reply(handle_search, &s) == CUVS_STATUS_METRIC_MISMATCH);
    s.index_relfilenode = 5009;
    CHECK("cagra search with another identity is refused NOT_CURRENT", reply(handle_search, &s) == CUVS_STATUS_NOT_CURRENT);
    s.index_relfilenode = 0;
    CHECK("cagra search with no identity is refused NOT_CURRENT", reply(handle_search, &s) == CUVS_STATUS_NOT_CURRENT);
    s.index_relfilenode = 5002; s.heap_relfilenode = 16390;
    CHECK("a search ignores the heap relfilenode (ALTER TABLE SET TABLESPACE keeps rows and TIDs)", reply(handle_search, &s) == CUVS_STATUS_METRIC_MISMATCH);
    {
        CuvsCmdFrame x = {.op = CUVS_OP_EXPORT_ADJACENCY, .db_oid = 1, .index_oid = 2, .index_relfilenode = 5002, .heap_relfilenode = 16390};
        CHECK("an export of the build for another heap relfilenode is refused NOT_CURRENT", reply(handle_export_adjacency, &x) == CUVS_STATUS_NOT_CURRENT);
        x.heap_relfilenode = 0;
        CHECK("an export without a heap identity is refused NOT_CURRENT", reply(handle_export_adjacency, &x) == CUVS_STATUS_NOT_CURRENT);
    }
    {
        /* the upper-layer export of a pg_cuvs_hnsw build from a cagra source is an export too: both relfilenodes must match */
        CuvsCmdFrame x = {.op = CUVS_OP_EXPORT_HNSW_UPPER, .db_oid = 1, .index_oid = 2, .index_relfilenode = 5002, .heap_relfilenode = 16390};
        CHECK("an upper-layer export of the build for another heap relfilenode is refused NOT_CURRENT", reply_isolated(handle_export_hnsw_upper, &x) == CUVS_STATUS_NOT_CURRENT);
        x.heap_relfilenode = 0;
        CHECK("an upper-layer export without a heap identity is refused NOT_CURRENT", reply_isolated(handle_export_hnsw_upper, &x) == CUVS_STATUS_NOT_CURRENT);
        x.heap_relfilenode = 16384; x.index_relfilenode = 5009;
        CHECK("an upper-layer export of the build for another index relfilenode is refused NOT_CURRENT", reply_isolated(handle_export_hnsw_upper, &x) == CUVS_STATUS_NOT_CURRENT);
    }

    CuvsCmdFrame v = {.op = CUVS_OP_SEARCH_IVFPQ, .db_oid = 1, .index_oid = 3, .dim = 4, .k = 1, .metric = CUVS_METRIC_IP};
    v.index_relfilenode = 5003; v.heap_relfilenode = 16384;
    CHECK("ivfpq search with the build's identity gets past the check", reply(handle_search_ivfpq, &v) == CUVS_STATUS_METRIC_MISMATCH);
    v.index_relfilenode = 5002;
    CHECK("ivfpq search with another index's identity is refused NOT_CURRENT", reply(handle_search_ivfpq, &v) == CUVS_STATUS_NOT_CURRENT);

    q->index_rfn = 0; q->heap_rfn = 0;   /* a build with no record */
    v.index_relfilenode = 0; v.heap_relfilenode = 0;
    CHECK("a build with no identity never matches, not even 0", reply(handle_search_ivfpq, &v) == CUVS_STATUS_NOT_CURRENT);

    q->index_rfn = 0; q->heap_rfn = 16384; q->identity_legacy = 1;   /* loaded with a record of an earlier version */
    v.index_relfilenode = 7777; v.heap_relfilenode = 16384;
    CHECK("a build with a record of an earlier version answers a request for its heap", reply(handle_search_ivfpq, &v) == CUVS_STATUS_METRIC_MISMATCH);
    v.heap_relfilenode = 16390;
    CHECK("and is refused for another heap", reply(handle_search_ivfpq, &v) == CUVS_STATUS_NOT_CURRENT);
    v.heap_relfilenode = 0;
    CHECK("and for a request without a heap identity", reply(handle_search_ivfpq, &v) == CUVS_STATUS_NOT_CURRENT);

    char path[512];
    FILE *f;
    IndexEntry r;
    snprintf(path, sizeof(path), "%s/1_2.relfilenode", dir);
    f = fopen(path, "w"); assert(f); fprintf(f, "16384 16380 5002 0\n"); fclose(f);
    read_index_identity(dir, 1, 2, &r);
    CHECK("a record of this version gives both relfilenodes", r.index_rfn == 5002 && r.heap_rfn == 16384 && !r.identity_legacy);
    f = fopen(path, "w"); assert(f); fprintf(f, "16384 16380\n"); fclose(f);
    read_index_identity(dir, 1, 2, &r);
    CHECK("a record of an earlier version (two fields) gives the heap only", r.index_rfn == 0 && r.heap_rfn == 16384 && r.identity_legacy);
    f = fopen(path, "w"); assert(f); fprintf(f, "16384 16380 0 0\n"); fclose(f);
    read_index_identity(dir, 1, 2, &r);
    CHECK("a record emptied in place is not taken for one of an earlier version", r.index_rfn == 0 && !r.identity_legacy);
    unlink(path);
    read_index_identity(dir, 1, 2, &r);
    CHECK("no record gives nothing", r.index_rfn == 0 && r.heap_rfn == 0 && !r.identity_legacy);
    rmdir(dir);

    if (failures) { fprintf(stderr, "test_build_identity_daemon: %d failure(s)\n", failures); return 1; }
    puts("PASS the daemon answers only for the index and heap relfilenodes its build was made for; an earlier record, for its heap");
    return 0;
}

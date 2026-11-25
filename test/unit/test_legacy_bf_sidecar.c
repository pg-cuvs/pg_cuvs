/* Exercise the legacy daemon paths with the CPU wrapper and real sidecars. */
#define _GNU_SOURCE
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#include <assert.h>

static const uint32_t db = 1, idx = 2, dim = 2;
static const float base_vecs[] = {1, 0, 2, 0, 3, 0, 4, 0};
static const uint64_t base_tids[] = {101, 102, 103, 104};

static uint32_t
reply_status(int sockets[2])
{
    CuvsReplyHeader reply;
    assert(recv_all(sockets[0], &reply, sizeof(reply)) == 0);
    close(sockets[0]);
    close(sockets[1]);
    return reply.status;
}

static uint32_t
build(const char *dir, const float *vecs, const uint64_t *tids)
{
    CuvsCmdFrame cmd = {.op = CUVS_OP_BUILD, .db_oid = db, .index_oid = idx,
                        .dim = dim, .n_vecs = 4, .metric = CUVS_METRIC_L2,
                        .graph_degree = 2};
    ExtraSearchRoot root = {.fd = -1};
    const char *io_dir = extra_search_build_dir(&cmd, dir, &root);
    char final[512], tmp[576];
    vectors_file_path(final, sizeof(final), io_dir, db, idx);
    snprintf(tmp, sizeof(tmp), "%s.tmp", final);
    FILE *vf = fopen(tmp, "wb");
    assert(vf);
    uint32_t crc = cuvs_crc32(tids, sizeof(base_tids));
    assert(cuvs_vectors_write(vf, cmd.n_vecs, dim, cmd.metric, crc, vecs) == 0);
    assert(fclose(vf) == 0);

    CuvsCagraIndex graph = cuvs_cagra_build(vecs, cmd.n_vecs, dim, cmd.metric,
                                          2, 3, CUVS_CAGRA_BUILD_AUTO, 0, 0, NULL);
    uint64_t *tids_copy = malloc(sizeof(base_tids));
    assert(graph && tids_copy);
    memcpy(tids_copy, tids, sizeof(base_tids));

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pthread_mutex_lock(&g_index_mutex);
    finish_build_commit(sockets[1], &cmd, dir, root.fd >= 0 ? &root : NULL,
                        graph, tids_copy, 0, estimate_vram_bytes(4, dim, 2),
                        1, tmp, final, crc, NULL, -1, NULL);
    if (root.fd >= 0)
        close(root.fd);
    return reply_status(sockets);
}

static void
compact(uint64_t dead_tid)
{
    char tombstone[512], dir[256] = {0};
    snprintf(dir, sizeof(dir), "%s", g_index_dir);
    snprintf(tombstone, sizeof(tombstone), "%s/%u_%u.tombstone", dir, db, idx);
    CuvsTombstoneHeader header = {.magic = CUVS_TOMBSTONE_MAGIC, .version = 1, .n_entries = 1};
    CuvsTombstoneRecord record = {.tid = dead_tid, .delete_xid = 1};
    FILE *file = fopen(tombstone, "wb");
    assert(file);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    assert(fwrite(&record, sizeof(record), 1, file) == 1);
    assert(fclose(file) == 0);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(cuvs_fd_send(sockets[0], -1, dir, sizeof(dir)) == 0);
    CuvsCmdFrame cmd = {.op = CUVS_OP_COMPACT, .db_oid = db, .index_oid = idx};
    handle_compact(sockets[1], &cmd);
    assert(reply_status(sockets) == CUVS_STATUS_OK);
}

static void
extend(uint64_t tid)
{
    struct { float vec[2]; uint64_t tid; } row = {{9, 0}, tid};
    char dir[256] = {0};
    snprintf(dir, sizeof(dir), "%s", g_index_dir);
    CuvsCmdFrame cmd = {.op = CUVS_OP_EXTEND, .db_oid = db, .index_oid = idx,
                        .dim = dim, .n_vecs = 1};
    snprintf(cmd.shm_key, sizeof(cmd.shm_key), "/bf-extend-%ld", (long)getpid());
    int fd = shm_open(cmd.shm_key, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, sizeof(row)) == 0);
    assert(write(fd, &row, sizeof(row)) == sizeof(row));
    close(fd);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(cuvs_fd_send(sockets[0], -1, dir, sizeof(dir)) == 0);
    handle_extend(sockets[1], &cmd);
    assert(shm_unlink(cmd.shm_key) == 0);
    assert(reply_status(sockets) == CUVS_STATUS_OK);
}

static void
assert_bf_hit(IndexEntry *e, float first, uint64_t tid)
{
    float query[] = {first, 0};
    CuvsSearchResult result;
    refresh_main_bf_cache(e, 0);
    assert(e->main_bf_idx);
    assert(cuvs_bf_search(e->main_bf_idx, query, dim, 1, &result, 0) == 0);
    assert(result.distance == 0 && result.item_id >= 0 && result.item_id < e->n_vecs);
    assert(e->tids[result.item_id] == tid);
}

static void
reused_tid_after_reload(void)
{
    assert(build(g_index_dir, base_vecs, base_tids) == CUVS_STATUS_OK);
    IndexEntry *e = find_index(db, idx);
    uint32_t before, after;
    assert_bf_hit(e, 4, 104);
    assert(tids_body_crc_of(e, &before) == 0);
    compact(104);
    extend(104);
    assert(e->n_vecs == 4 && memcmp(e->tids, base_tids, sizeof(base_tids)) == 0);

    int persist_failed = 0;
    pthread_mutex_lock(&g_index_mutex);
    (void) evict_entry(e, 0, CUVS_EVICT_MANUAL_UNLOAD, 0, CUVS_RQ_MANUAL, &persist_failed, true);
    assert(!persist_failed && find_index(db, idx) == NULL);
    assert(load_index(db, idx, CUVS_RQ_LOAD) == 0);
    pthread_mutex_unlock(&g_index_mutex);
    e = find_index(db, idx);
    assert(e && e->handle && e->n_vecs == 4);
    assert(tids_body_crc_of(e, &after) == 0 && before == after);
    refresh_main_bf_cache(e, 0);
    assert(!e->main_bf_idx);
}

static void
same_count_streaming(void)
{
    assert(build(g_index_dir, base_vecs, base_tids) == CUVS_STATUS_OK);
    IndexEntry *e = find_index(db, idx);
    float query[] = {4, 0};
    uint64_t tid = 104;
    CuvsResult result;
    int found;
    assert(search_filter_exact(e, query, &tid, 1, 1, 1, &result, &found) == CUVS_STATUS_OK);
    assert(found == 1 && result.tid == tid && result.distance == 0);
    extend(105);
    compact(101);
    assert(e->n_vecs == 4 && e->tids[3] == 105);
    query[0] = 9;
    tid = 105;
    assert(search_filter_exact(e, query, &tid, 1, 1, 1, &result, &found) == CUVS_STATUS_NO_VECTORS);
    assert(found == 0);
}

static void
alias_directory(void)
{
    char alias[sizeof(g_index_dir) + sizeof("/.")];
    snprintf(alias, sizeof(alias), "%s/.", g_index_dir);
    assert(build(alias, base_vecs, base_tids) == CUVS_STATUS_OK);
    IndexEntry *e = find_index(db, idx);
    assert_bf_hit(e, 4, 104);
    const float replacement[] = {1, 0, 2, 0, 3, 0, 9, 0};
    assert(build(alias, replacement, base_tids) == CUVS_STATUS_OK);
    assert_bf_hit(e, 9, 104);
}

static void
failed_rebuild_rollback(void)
{
    assert(build(g_index_dir, base_vecs, base_tids) == CUVS_STATUS_OK);
    IndexEntry *e = find_index(db, idx);
    assert_bf_hit(e, 4, 104);
    uint32_t before, after;
    assert(tids_body_crc_of(e, &before) == 0);
    const uint64_t new_tids[] = {201, 202, 203, 204};
    assert(setenv("CUVS_FAULT_RENAME_CAGRA", "1", 1) == 0);
    assert(build(g_index_dir, base_vecs, new_tids) == CUVS_STATUS_PERSIST_FAILED);
    assert(unsetenv("CUVS_FAULT_RENAME_CAGRA") == 0);
    assert(tids_body_crc_of(e, &after) == 0 && before == after);
    assert_bf_hit(e, 4, 104);
}

int
main(int argc, char **argv)
{
    assert(argc == 2);
    assert(strlen(argv[1]) > 0);
    char dir[] = "/tmp/legacy-bf-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(g_index_dir, sizeof(g_index_dir), "%s", dir);
    g_max_indexes = 4;
    g_indexes = calloc((size_t)g_max_indexes, sizeof(*g_indexes));
    assert(g_indexes);
    g_n_gpus = 1;
    g_peer_uid = getuid();

    if (strcmp(argv[1], "reused-tid") == 0)
        reused_tid_after_reload();
    else if (strcmp(argv[1], "streaming") == 0)
        same_count_streaming();
    else if (strcmp(argv[1], "alias") == 0)
        alias_directory();
    else if (strcmp(argv[1], "rollback") == 0)
        failed_rebuild_rollback();
    else
        return 2;

    (void) evict_entry(find_index(db, idx), 0, CUVS_EVICT_MANUAL_UNLOAD, 0, CUVS_RQ_MANUAL, NULL, false);
    free(g_indexes);
    char path[512];
    index_file_path(path, sizeof(path), dir, db, idx);
    assert(unlink(path) == 0);
    tids_file_path(path, sizeof(path), dir, db, idx);
    assert(unlink(path) == 0);
    vectors_file_path(path, sizeof(path), dir, db, idx);
    assert(unlink(path) == 0 || errno == ENOENT);
    assert(rmdir(dir) == 0);
    printf("PASS legacy BF sidecar: %s\n", argv[1]);
    return 0;
}

/* Exercise the legacy daemon paths with the CPU wrapper and real sidecars. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

static int fixture_fsync(int fd);
static int fixture_rename(const char *from, const char *to);
static int fixture_unlink(const char *path);
#define fsync fixture_fsync
#define rename fixture_rename
#define unlink fixture_unlink
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main
#undef unlink
#undef rename
#undef fsync
#include <assert.h>
#include <sys/wait.h>

static const uint32_t db = 1, idx = 2, dim = 2;
static const float base_vecs[] = {1, 0, 2, 0, 3, 0, 4, 0};
static const float replacement_vecs[] = {1, 0, 2, 0, 3, 0, 9, 0};
static const uint64_t base_tids[] = {101, 102, 103, 104};

typedef enum {
    NO_FAULT, BUILD_CRASH, BUILD_SYNC_FAIL, COMPACT_UNLINK_FAIL, COMPACT_SYNC_FAIL,
    COMPACT_PUBLISH_SYNC_FAIL, COMPACT_TOMBSTONE_UNLINK_FAIL
} FixtureFault;
static FixtureFault fault;
static bool graph_renamed;
enum { EXIT_AFTER_GRAPH_SYNC = 37 };

static bool
has_suffix(const char *path, const char *suffix)
{
    size_t len = strlen(path), suffix_len = strlen(suffix);
    return len >= suffix_len && strcmp(path + len - suffix_len, suffix) == 0;
}

static int
fixture_unlink(const char *path)
{
    if ((fault == COMPACT_UNLINK_FAIL && has_suffix(path, ".vectors")) ||
        (fault == COMPACT_TOMBSTONE_UNLINK_FAIL && has_suffix(path, ".tombstone")))
    {
        errno = EACCES;
        return -1;
    }
    return unlink(path);
}

static int
fixture_rename(const char *from, const char *to)
{
    int rc = rename(from, to);
    if (rc == 0 && (fault == BUILD_CRASH || fault == BUILD_SYNC_FAIL ||
                    fault == COMPACT_PUBLISH_SYNC_FAIL) && has_suffix(to, ".cagra"))
        graph_renamed = true;
    return rc;
}

static int
fixture_fsync(int fd)
{
    struct stat st;
    bool directory = fault != NO_FAULT && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
    if (directory && (fault == COMPACT_SYNC_FAIL ||
                      (graph_renamed && (fault == BUILD_SYNC_FAIL ||
                                         fault == COMPACT_PUBLISH_SYNC_FAIL))))
    {
        if (fault == BUILD_SYNC_FAIL)
            fault = NO_FAULT; /* The rollback can still sync its restored files. */
        errno = EIO;
        return -1;
    }
    int rc = fsync(fd);
    if (rc == 0 && directory && fault == BUILD_CRASH && graph_renamed)
        _exit(EXIT_AFTER_GRAPH_SYNC);
    return rc;
}

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
write_tombstone(uint64_t dead_tid)
{
    char tombstone[512];
    snprintf(tombstone, sizeof(tombstone), "%s/%u_%u.tombstone", g_index_dir, db, idx);
    CuvsTombstoneHeader header = {.magic = CUVS_TOMBSTONE_MAGIC, .version = 1, .n_entries = 1};
    CuvsTombstoneRecord record = {.tid = dead_tid, .delete_xid = 1};
    FILE *file = fopen(tombstone, "wb");
    assert(file);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    assert(fwrite(&record, sizeof(record), 1, file) == 1);
    assert(fclose(file) == 0);
}

static uint32_t
run_compact(void)
{
    char dir[256] = {0};
    snprintf(dir, sizeof(dir), "%s", g_index_dir);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(cuvs_fd_send(sockets[0], -1, dir, sizeof(dir)) == 0);
    CuvsCmdFrame cmd = {.op = CUVS_OP_COMPACT, .db_oid = db, .index_oid = idx};
    handle_compact(sockets[1], &cmd);
    return reply_status(sockets);
}

static void
compact(uint64_t dead_tid)
{
    write_tombstone(dead_tid);
    assert(run_compact() == CUVS_STATUS_OK);
}

static uint32_t
run_extend(uint64_t tid)
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
    return reply_status(sockets);
}

static void
extend(uint64_t tid)
{
    assert(run_extend(tid) == CUVS_STATUS_OK);
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
    assert(build(alias, replacement_vecs, base_tids) == CUVS_STATUS_OK);
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
    free_main_bf_cache(e); /* Verify the restored .vectors with a cold cache. */
    assert_bf_hit(e, 4, 104);
}

static IndexEntry *
reload_from_disk(void)
{
    pthread_mutex_lock(&g_index_mutex);
    (void) evict_entry(find_index(db, idx), 0, CUVS_EVICT_MANUAL_UNLOAD, 0,
                       CUVS_RQ_MANUAL, NULL, false);
    assert(find_index(db, idx) == NULL);
    assert(load_index(db, idx, CUVS_RQ_LOAD) == 0);
    pthread_mutex_unlock(&g_index_mutex);
    return find_index(db, idx);
}

static void
assert_graph_hit(IndexEntry *e, float first, uint64_t tid)
{
    float query[] = {first, 0};
    CuvsSearchResult result;
    assert(e && e->handle);
    assert(cuvs_cagra_search(e->handle, query, dim, 1, &result, 0, 0, 0) == 0);
    assert(result.distance == 0 && result.item_id >= 0 && result.item_id < e->n_vecs);
    assert(e->tids[result.item_id] == tid);
}

static void
cleanup_interrupted_build(void)
{
    DIR *dir = opendir(g_index_dir);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strncmp(entry->d_name, ".cagra-build-", strlen(".cagra-build-")) != 0)
            continue;
        char backup[576];
        snprintf(backup, sizeof(backup), "%s/%s", g_index_dir, entry->d_name);
        const char *names[] = {"tids", "graph", "vectors"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        {
            char path[592];
            snprintf(path, sizeof(path), "%s/%s", backup, names[i]);
            assert(unlink(path) == 0 || errno == ENOENT);
        }
        assert(rmdir(backup) == 0);
    }
    closedir(dir);

    char vectors[512], tmp[576];
    vectors_file_path(vectors, sizeof(vectors), g_index_dir, db, idx);
    snprintf(tmp, sizeof(tmp), "%s.tmp", vectors);
    assert(unlink(tmp) == 0);
}

static void
rebuild_crash_after_base_sync(void)
{
    assert(build(g_index_dir, base_vecs, base_tids) == CUVS_STATUS_OK);
    IndexEntry *e = find_index(db, idx);
    assert_bf_hit(e, 4, 104);
    uint32_t before, after;
    assert(tids_body_crc_of(e, &before) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
    {
        fault = BUILD_CRASH;
        (void) build(g_index_dir, replacement_vecs, base_tids);
        _exit(1); /* The sync checkpoint was not reached. */
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_AFTER_GRAPH_SYNC);

    e = reload_from_disk();
    assert_graph_hit(e, 9, 104);
    assert(tids_body_crc_of(e, &after) == 0 && before == after);
    refresh_main_bf_cache(e, 0);
    assert(!e->main_bf_idx);
    cleanup_interrupted_build();
}

static void
failed_build_fsync_rollback(void)
{
    assert(build(g_index_dir, base_vecs, base_tids) == CUVS_STATUS_OK);
    assert_bf_hit(find_index(db, idx), 4, 104);
    char alias[sizeof(g_index_dir) + sizeof("/.")];
    snprintf(alias, sizeof(alias), "%s/.", g_index_dir);
    const uint64_t new_tids[] = {201, 202, 203, 204};
    fault = BUILD_SYNC_FAIL;
    assert(build(alias, replacement_vecs, new_tids) == CUVS_STATUS_PERSIST_FAILED);
    assert(graph_renamed && fault == NO_FAULT);

    IndexEntry *e = reload_from_disk();
    assert_graph_hit(e, 4, 104);
    assert_bf_hit(e, 4, 104);
}

static void
compact_save_retry(FixtureFault save_fault)
{
    assert(build(g_index_dir, base_vecs, base_tids) == CUVS_STATUS_OK);
    IndexEntry *e = find_index(db, idx);
    char tombstone[512];
    snprintf(tombstone, sizeof(tombstone), "%s/%u_%u.tombstone", g_index_dir, db, idx);
    uint32_t before, after;
    assert(tids_body_crc_of(e, &before) == 0);
    CuvsCagraIndex old_graph = e->handle;
    uint64_t *old_tids = e->tids;
    bool published = save_fault == COMPACT_PUBLISH_SYNC_FAIL ||
                     save_fault == COMPACT_TOMBSTONE_UNLINK_FAIL;
    write_tombstone(104);
    fault = save_fault;
    graph_renamed = false;
    assert(run_compact() == CUVS_STATUS_PERSIST_FAILED);
    if (published)
        assert(e->n_vecs == 3 && e->compact_count == 1);
    else
    {
        assert(e->handle == old_graph && e->tids == old_tids);
        assert(e->n_vecs == 4 && e->compact_count == 0 && e->file_current);
        assert_graph_hit(e, 4, 104);
    }
    assert(access(tombstone, F_OK) == 0);

    /* No new vector or eviction while the retry can still see the old TID. */
    CuvsCagraIndex retained = e->handle;
    assert(run_extend(104) == CUVS_STATUS_STALE);
    assert(e->handle == retained && e->n_extended == 0);
    int persist_failed = 0;
    assert(evict_entry(e, 0, CUVS_EVICT_MANUAL_UNLOAD, 0,
                       CUVS_RQ_MANUAL, &persist_failed, true) == 0);
    assert(persist_failed && find_index(db, idx) == e);
    assert(save_index(e) != 0);
    graph_renamed = false;
    assert(run_compact() == CUVS_STATUS_PERSIST_FAILED);
    assert(access(tombstone, F_OK) == 0);
    assert(tids_body_crc_of(e, &after) == 0 && (before != after) == published);
    assert(e->handle == retained);

    fault = NO_FAULT;
    assert(run_compact() == CUVS_STATUS_OK);
    assert(e->n_vecs == 3 && e->compact_count == 1 && e->file_current);
    assert(access(tombstone, F_OK) != 0 && errno == ENOENT);
    e = reload_from_disk();
    assert(e->n_vecs == 3 && e->tids[2] == 103);
    assert_graph_hit(e, 3, 103);
    extend(104);
    assert(save_index(e) == 0);
    e = reload_from_disk();
    assert_graph_hit(e, 9, 104);
}

static void
binding_after_other_dir_rollback(void)
{
    assert(build(g_index_dir, base_vecs, base_tids) == CUVS_STATUS_OK);
    IndexEntry *e = find_index(db, idx);
    char hnsw[512], tids[512], binding_path[576], other_dir[576];
    hnsw_file_path(hnsw, sizeof(hnsw), g_index_dir, db, idx);
    tids_file_path(tids, sizeof(tids), g_index_dir, db, idx);
    snprintf(binding_path, sizeof(binding_path), "%s%s", hnsw, CUVS_HNSW_SOURCE_SUFFIX);
    snprintf(other_dir, sizeof(other_dir), "%s/other-XXXXXX", g_index_dir);
    assert(mkdtemp(other_dir));

    FILE *file = fopen(hnsw, "wb");
    assert(file);
    assert(fputs("binding artifact", file) >= 0 && fclose(file) == 0);
    assert(bind_hnsw_source(g_index_dir, hnsw, tids, NULL) == 0);
    CuvsHnswSourceBinding before, after;
    assert(read_hnsw_source(hnsw, tids, &before));
    assert(setenv("CUVS_FAULT_RENAME_CAGRA", "1", 1) == 0);
    assert(build(other_dir, replacement_vecs, base_tids) == CUVS_STATUS_PERSIST_FAILED);
    assert(unsetenv("CUVS_FAULT_RENAME_CAGRA") == 0);
    assert(read_hnsw_source(hnsw, tids, &after) && before.tids.inode == after.tids.inode);
    assert(save_index(e) == 0);
    assert(read_hnsw_source(hnsw, tids, &after) && before.tids.inode != after.tids.inode);

    assert(unlink(binding_path) == 0);
    assert(unlink(hnsw) == 0);
    assert(rmdir(other_dir) == 0);
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
    else if (strcmp(argv[1], "build-crash") == 0)
        rebuild_crash_after_base_sync();
    else if (strcmp(argv[1], "build-fsync-rollback") == 0)
        failed_build_fsync_rollback();
    else if (strcmp(argv[1], "compact-retry-unlink") == 0)
        compact_save_retry(COMPACT_UNLINK_FAIL);
    else if (strcmp(argv[1], "compact-retry-fsync") == 0)
        compact_save_retry(COMPACT_SYNC_FAIL);
    else if (strcmp(argv[1], "compact-retry-publish") == 0)
        compact_save_retry(COMPACT_PUBLISH_SYNC_FAIL);
    else if (strcmp(argv[1], "compact-retry-tombstone") == 0)
        compact_save_retry(COMPACT_TOMBSTONE_UNLINK_FAIL);
    else if (strcmp(argv[1], "binding-other-dir") == 0)
        binding_after_other_dir_rollback();
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

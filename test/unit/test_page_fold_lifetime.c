/* The real daemon registry and mutex are exercised with a CPU shim.
 * Only the CAGRA destructor is observed through the test linker.
 */
#define _GNU_SOURCE
#define main pg_cuvs_server_program_main
#include "../../src/pg_cuvs_server.c"
#undef main

typedef struct FoldHandleProbe
{
    int frees;
    bool free_outside_registry;
    size_t bytes_during_free;
#ifdef CUVS_CAGRA_FOLD_ENABLED
    unsigned recovery_owners_during_free;
#endif
} FoldHandleProbe;

#define CHECK(condition, message) \
    do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); exit(1); } } while (0)

void __wrap_cuvs_cagra_free(CuvsCagraIndex index, int device);

void
__wrap_cuvs_cagra_free(CuvsCagraIndex index, int device)
{
    FoldHandleProbe *probe = index;
    int locked;

    CHECK(probe && device == 0, "destructor used the wrong handle or device");
    ++probe->frees;
    locked = pthread_mutex_trylock(&g_index_mutex);
    CHECK(locked == 0 || locked == EBUSY, "unexpected registry mutex result");
    if (locked == 0)
    {
        probe->free_outside_registry = true;
        probe->bytes_during_free = g_page_graph_bytes[device];
#ifdef CUVS_CAGRA_FOLD_ENABLED
        probe->recovery_owners_during_free = g_page_recovery_owners;
#endif
        pthread_mutex_unlock(&g_index_mutex);
    }
}

/* Fixture helper. As in the daemon, zero initialization starts a new graph
 * with no external references, and registry accounting is under the mutex. */
static PageCagraGraph *
register_graph(FoldHandleProbe *handle, uint64_t nonce, size_t bytes)
{
    PageCagraGraph *entry = calloc(1, sizeof(*entry));

    CHECK(entry, "allocate registry entry");
    entry->system_identifier = 1;
    entry->db_oid = 2;
    entry->index_oid = 3;
    entry->index_nonce = 4;
    entry->graph_nonce = nonce;
    entry->n_slots = 64;
    entry->handle = handle;
    entry->bytes = bytes;
    entry->next = g_page_graphs;
    g_page_graphs = entry;
    g_page_graph_bytes[0] += bytes;
    ++g_n_page_graphs;
    return entry;
}

static PageCagraGraph *
lookup(uint64_t nonce)
{
    CuvsCmdFrame cmd = {.db_oid = 2, .index_oid = 3};
    CuvsPageSearchRequest request = {
        .system_identifier = 1, .index_nonce = 4,
        .base_nonce = nonce, .graph_nonce = nonce, .n_slots = 64
    };

    return find_page_graph(&cmd, &request);
}

typedef struct FoldGate
{
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool pinned;
    bool finish;
    FoldHandleProbe *expected;
} FoldGate;

static void *
fold_worker(void *data)
{
    FoldGate *gate = data;
    PageGraphFoldRef ref = {0};
    CuvsCagraIndex source;

    pthread_mutex_lock(&g_index_mutex);
    source = page_graph_fold_pin_locked(lookup(5), &ref);
    pthread_mutex_unlock(&g_index_mutex);
    CHECK(source == gate->expected, "worker did not pin the exact source");
    pthread_mutex_lock(&gate->mutex);
    gate->pinned = true;
    pthread_cond_signal(&gate->changed);
    while (!gate->finish)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
    CHECK(gate->expected->frees == 0, "borrowed handle freed during unlocked fold");
    page_graph_fold_unpin(&ref);
    return NULL;
}

static void
test_drop_during_two_pins(void)
{
    FoldHandleProbe old = {0}, replacement = {0};
    FoldGate gate = {
        .mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER,
        .expected=&old
    };
    pthread_t worker;
    PageGraphFoldRef second = {0};
    PageGraphFoldRef late = {0};
    PageCagraGraph *entry;

    pthread_mutex_lock(&g_index_mutex);
    entry = register_graph(&old, 5, 8192);
    CHECK(page_graph_fold_pin_locked(entry, &second) == &old, "pin old graph");
    CHECK(page_graph_fold_pin_locked(entry, &second) == NULL, "same pin token acquired twice");
    pthread_mutex_unlock(&g_index_mutex);

    CHECK(pthread_create(&worker, NULL, fold_worker, &gate)==0, "start fold worker");
    pthread_mutex_lock(&gate.mutex);
    while (!gate.pinned)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);

    pthread_mutex_lock(&g_index_mutex);
    drop_page_graphs(2, 3);
    CHECK(old.frees == 0, "DROP freed graph before its two fold references ended");
    CHECK(entry->search_invalidated, "DROP left the old resident cursor usable");
    CHECK(!lookup(5) && !g_page_graphs && g_n_page_graphs == 0, "retired graph remains visible");
    CHECK(g_page_graph_bytes[0] == 8192, "DROP deducted VRAM while graph still pinned");
    CHECK(page_graph_fold_pin_locked(entry, &late) == NULL, "retired graph can be pinned again");
    register_graph(&replacement, 6, 4096);
    CHECK(lookup(6) && g_n_page_graphs == 1, "replacement not registered");
    pthread_mutex_unlock(&g_index_mutex);

    pthread_mutex_lock(&gate.mutex);
    gate.finish = true;
    pthread_cond_signal(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    CHECK(pthread_join(worker, NULL) == 0, "join fold worker");
    CHECK(old.frees == 0, "first release freed graph with a live reference");
    CHECK(g_page_graph_bytes[0] == 12288, "first release deducted retired VRAM");

    page_graph_fold_unpin(&second);
    CHECK(old.frees == 1, "last reference did not free the old handle exactly once");
    CHECK(old.free_outside_registry, "deferred GPU destructor ran under registry lock");
    CHECK(old.bytes_during_free == 12288, "departing graph not charged during GPU release");
    CHECK(replacement.frees == 0, "last release freed replacement instead of old graph");
    CHECK(g_page_graph_bytes[0] == 4096, "last release did not refund only retired VRAM");
    page_graph_fold_unpin(&second);
    CHECK(old.frees == 1, "empty release called destructor twice");

    pthread_mutex_lock(&g_index_mutex);
    CHECK(lookup(6) && lookup(6)->handle == &replacement, "replacement lookup changed");
    drop_page_graphs(2, 3);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    release_retired_page_overlays();
    CHECK(replacement.frees == 1 && !g_page_graph_bytes[0] && !g_n_page_graphs,
          "replacement cleanup broke registry accounting");

    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
    puts("PASS DROP during unlocked fold, exact old graph retired on last of two releases");
}

static void
test_pinned_graph_is_not_reclaimable(void)
{
    FoldHandleProbe old = {0}, reclaimable = {0};
    PageGraphFoldRef ref = {0};
    PageCagraGraph *entry;
    CuvsEvictPick pick;

    pthread_mutex_lock(&g_index_mutex);
    g_reclaim.scope.kind = CUVS_SCOPE_LRU;
    entry = register_graph(&old, 5, 8192);
    register_graph(&reclaimable, 6, 4096);
    CHECK(page_graph_fold_pin_locked(entry, &ref) == &old, "pin pressured graph");
    CHECK(make_room(0, CUVS_RQ_LOAD, 8192, 0, &pick, false, false) != 0,
          "capacity picker counted pinned bytes as reclaimable");
    CHECK(!old.frees && !reclaimable.frees && g_page_graph_bytes[0] == 12288,
          "failed room pick freed a graph");
    CHECK(make_room(0, CUVS_RQ_LOAD, 4096, 0, &pick, false, false) == 0,
          "room picker failed to reclaim the unpinned graph");
    CHECK(!old.frees && reclaimable.frees == 1 && g_page_graph_bytes[0] == 8192,
          "room picker reclaimed the wrong graph");
    CHECK(!evict_page_graph(0) && !old.frees, "direct eviction selected a pinned graph");
    pthread_mutex_unlock(&g_index_mutex);

    page_graph_fold_unpin(&ref);
    CHECK(!old.frees && g_page_graph_bytes[0] == 8192, "unpin retired a registered graph");
    pthread_mutex_lock(&g_index_mutex);
    CHECK(lookup(5) == entry && evict_page_graph(0), "released registered graph was not evictable");
    pthread_mutex_unlock(&g_index_mutex);
    CHECK(old.frees == 1 && !g_page_graph_bytes[0] && !g_n_page_graphs,
          "eviction did not free the registered graph correctly");
    puts("PASS pinned graph excluded from capacity eviction; registered last unpin preserves graph");
}

#ifdef CUVS_CAGRA_FOLD_ENABLED
static void
test_recovery_charges_pinned_previous_boot(void)
{
    FoldHandleProbe old = {0}, replacement = {0};
    PageGraphFoldRef pin = {0}, late = {0};
    CuvsCmdFrame cmd = {.db_oid = 2, .index_oid = 3, .dim = 4};
    CuvsPageRecoveryRequest request = {
        .system_identifier = 1, .index_nonce = 4, .base_nonce = 5, .boot_id = 7,
        .target = {.graph_nonce = 5, .n_slots = 64},
    };
    const size_t needed = 8000;

    g_n_gpus = 1;
    g_max_vram_per_gpu[0] = 9000;
    pthread_mutex_lock(&g_index_mutex);
    PageCagraGraph *entry = register_graph(&old, 5, 4096);
    entry->base_nonce = 5;
    entry->producer_boot = 6;
    entry->dim = 4;
    entry->overlay.boot_id = 6;
    CHECK(!page_recovery_supersedes(entry, &cmd, &request, true),
          "boot change alone retired a usable exact graph");
    entry->placeholder_boot = 6;
    CHECK(page_graph_fold_pin_locked(entry, &pin) == &old, "pin old boot's exact instance");
    CHECK(!page_recovery_serving_locked(&cmd, &request), "boot-bound graph served a new PG life");
    CHECK(retire_page_recovery_locked(&cmd, &request, true) == 1 && !lookup(5),
          "authorized previous boot did not leave the serving registry");
    CHECK(page_graph_fold_pin_locked(entry, &late) == NULL, "retired instance accepted a new pin");
    CHECK(total_vram_used(0) == 4096 && page_recovery_device_locked(needed) < 0,
          "admission counted a pinned retirement as freed VRAM");
    pthread_mutex_unlock(&g_index_mutex);

    release_retired_fold_graphs();
    CHECK(!old.frees, "drain freed a still-pinned previous boot");
    page_graph_fold_unpin(&pin);
    CHECK(old.frees == 1 && old.free_outside_registry && old.bytes_during_free == 4096,
          "last pin did not free the old graph while it stayed charged");
    pthread_mutex_lock(&g_index_mutex);
    CHECK(!total_vram_used(0) && page_recovery_device_locked(needed) == 0,
          "released VRAM did not fit the same claim on retry");

    page_recovery_reserve_locked(0, needed);
    CHECK(total_vram_used(0) == needed && g_page_recovery_active[0], "recovery claim was not charged");
    page_recovery_finish_claim_locked(0, needed, 0);
    CHECK(!total_vram_used(0) && !g_page_recovery_active[0] && !g_pending_build_count[0],
          "abandoned preparation retained a reservation");
    page_recovery_reserve_locked(0, needed);
    entry = register_graph(&replacement, 5, 0);
    entry->bytes = 4096;
    page_recovery_finish_claim_locked(0, needed, entry->bytes);
    CHECK(total_vram_used(0) == 4096 && !g_page_recovery_active[0] && !g_pending_build_count[0],
          "publication did not atomically convert the claim to resident bytes");
    drop_page_graphs(2, 3);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    CHECK(replacement.frees == 1 && !g_page_graph_bytes[0], "replacement disposition broke accounting");
}

static void
test_recovery_keeps_current_boot_candidate(void)
{
    FoldHandleProbe parent_handle = {0}, offered = {0};
    CuvsCmdFrame cmd = {.db_oid = 2, .index_oid = 3, .dim = 4};
    CuvsPageRecoveryRequest request = {
        .system_identifier = 1, .index_nonce = 4, .base_nonce = 5, .boot_id = 7,
        .target = {.graph_nonce = 6, .n_slots = 68, .folded = 4},
    };

    pthread_mutex_lock(&g_index_mutex);
    PageCagraGraph *parent = register_graph(&parent_handle, 5, 4096);
    parent->base_nonce = 5;
    parent->dim = 4;
    parent->producer_boot = 6;
    parent->overlay.boot_id = 7;
    PageCagraGraph *candidate = calloc(1, sizeof(*candidate));
    CHECK(candidate, "allocate fold candidate");
    *candidate = (PageCagraGraph) {
        .system_identifier = 1, .db_oid = 2, .index_oid = 3, .index_nonce = 4,
        .base_nonce = 5, .graph_nonce = 6, .n_slots = 68, .folded = 4, .dim = 4,
        .fold_event = {.boot_id = 7}, .handle = &offered, .bytes = 8192,
    };
    parent->fold_candidate = candidate;
    g_page_graph_bytes[0] += candidate->bytes;
    CHECK(page_recovery_serving_locked(&cmd, &request), "restore ignored the current boot's published offer");
    CHECK(!retire_page_recovery_locked(&cmd, &request, true) &&
          !retire_page_recovery_locked(&cmd, &request, false) && parent->fold_candidate == candidate,
          "retirement invalidated the current boot's fold candidate");
    drop_page_graphs(2, 3);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    CHECK(parent_handle.frees == 1 && offered.frees == 1 && !g_page_graph_bytes[0],
          "retiring the fixture did not refund the parent and pending child exactly once");
}

static void *
page_drop_worker(void *arg)
{
    int fd = *(int *) arg;
    CuvsCmdFrame cmd = {.op = CUVS_OP_DROP_PAGES_INDEX, .db_oid = 2, .index_oid = 3};

    handle_drop(fd, &cmd);
    release_retired_fold_graphs();
    release_retired_page_overlays();
    close(fd);
    return NULL;
}

static void
test_recovery_retires_only_previous_boot_history(void)
{
    FoldHandleProbe earlier = {0}, offered = {0}, live = {0};
    PageGraphFoldRef held = {0};
    CuvsCmdFrame cmd = {.db_oid = 2, .index_oid = 3, .dim = 4};
    CuvsPageRecoveryRequest request = {
        .system_identifier = 1, .index_nonce = 4, .base_nonce = 5, .boot_id = 7,
        .target = {.graph_nonce = 8, .n_slots = 72, .folded = 8},
    };

    pthread_mutex_lock(&g_index_mutex);
    PageCagraGraph *previous = register_graph(&earlier, 5, 4096);
    PageCagraGraph *current = register_graph(&live, 6, 8192);

    previous->base_nonce = 5;
    previous->overlay.boot_id = 6;
    current->base_nonce = 5;
    current->overlay.boot_id = 7;
    previous->fold_candidate = calloc(1, sizeof(*previous->fold_candidate));
    CHECK(previous->fold_candidate, "allocate previous boot's fold candidate");
    previous->fold_candidate->handle = &offered;
    previous->fold_candidate->bytes = 4096;
    g_page_graph_bytes[0] += 4096;
    CHECK(page_graph_fold_pin_locked(previous, &held) == &earlier, "pin earlier history");
    g_page_ancestors = g_page_graphs;
    g_page_graphs = NULL;
    g_n_page_graphs = 0;
    retire_recovery_ancestors_locked(&cmd, &request);
    CHECK(g_page_ancestors == current && !current->next && !current->search_invalidated,
          "recovery disturbed current boot history");
    CHECK(previous->search_invalidated && previous->fold_retired &&
          g_page_graph_bytes[0] == 16384 && !earlier.frees && !offered.frees,
          "recovery did not preserve the charge of the retired history pin");
    pthread_mutex_unlock(&g_index_mutex);

    release_retired_fold_graphs();
    CHECK(offered.frees == 1 && offered.recovery_owners_during_free == 2 &&
          g_page_recovery_owners == 1 && !earlier.frees,
          "recovery stopped owning the deferred candidate before its GPU free");
    page_graph_fold_unpin(&held);
    CHECK(earlier.frees == 1 && earlier.bytes_during_free == 12288 &&
          g_page_graph_bytes[0] == 8192, "history charge was refunded before the last pin");
    CHECK(earlier.recovery_owners_during_free == 1 && g_page_recovery_owners == 0,
          "recovery must own the graph until the last pin's GPU free returns");
    pthread_mutex_lock(&g_index_mutex);
    prune_page_ancestors_locked(2, 3, 1, 4, 5, 7, 0);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    CHECK(live.frees == 1 && !g_page_graph_bytes[0] && !g_page_ancestors,
          "history fixture did not release the current boot graph");
}

static void
test_page_drop_waits_for_legacy_export(void)
{
    char path[] = "/tmp/pg-cuvs-drop-export.XXXXXX";
    char request[CUVS_PAGE_INDEX_DIR_SIZE] = {0};
    FoldHandleProbe legacy = {0}, page = {0};
    CuvsReplyHeader reply;
    PageStorageScope *scope;
    pthread_t worker;
    int peers[2];

    CHECK(mkdtemp(path), "create private DROP directory");
    snprintf(request, sizeof(request), "%s", path);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, peers) == 0, "create DROP peers");
    pthread_mutex_lock(&g_index_mutex);
    g_indexes = calloc(1, sizeof(*g_indexes));
    CHECK(g_indexes, "allocate legacy registry");
    g_indexes[0] = (IndexEntry) {.db_oid = 2, .index_oid = 3, .valid = 1,
                                .handle = &legacy, .inflight = 1};
    g_n_indexes = 1;
    register_graph(&page, 5, 4096);
    pthread_mutex_unlock(&g_index_mutex);

    CHECK(send_all(peers[0], request, sizeof(request)) == 0, "send page DROP root");
    CHECK(pthread_create(&worker, NULL, page_drop_worker, &peers[1]) == 0,
          "start page DROP with the legacy export in flight");
    for (;;)
    {
        pthread_mutex_lock(&g_index_mutex);
        if (g_n_page_graphs == 0)
            break;
        pthread_mutex_unlock(&g_index_mutex);
        (void) poll(NULL, 0, 1);
    }
    CHECK(legacy.frees == 0 && g_n_indexes == 1 && g_indexes[0].inflight == 1,
          "DROP reclaimed the borrowed legacy graph");
    --g_indexes[0].inflight;
    pthread_mutex_unlock(&g_index_mutex);

    CHECK(recv_all(peers[0], &reply, sizeof(reply)) == 0 && reply.status == CUVS_STATUS_OK,
          "committed page DROP did not resume after the legacy export");
    CHECK(pthread_join(worker, NULL) == 0, "join page DROP");
    CHECK(g_n_indexes == 0 && legacy.frees == 1 && page.frees == 1,
          "committed DROP did not reclaim both graphs exactly once");
    scope = page_storage_get(2, 3);
    CHECK(scope && pthread_mutex_trylock(&scope->gate) == 0,
          "page DROP leaked the storage gate");
    pthread_mutex_unlock(&scope->gate);
    page_storage_put(scope);
    close(peers[0]);
    free(g_indexes);
    g_indexes = NULL;
    CHECK(rmdir(path) == 0, "remove private DROP directory");
    puts("PASS committed page DROP resumes and releases its gate after legacy export");
}
static void
test_checkpoint_crc_blocks(void)
{
    const off_t bytes = (off_t) PAGE_CHECKPOINT_CRC_BLOCK_BYTES + 127;
    const char pattern[] = "page-checkpoint-crc";
    CuvsPageManifest manifest = {.artifact_bytes = (uint64_t) bytes};
    FILE *file = tmpfile();
    char path[64];
    int fd;

    CHECK(file, "create private checkpoint CRC fixture");
    fd = fileno(file);
    CHECK(ftruncate(fd, bytes) == 0, "size checkpoint fixture with a partial final block");
    CHECK(pwrite(fd, pattern, sizeof(pattern), (off_t) PAGE_CHECKPOINT_CRC_BLOCK_BYTES - 3) ==
          (ssize_t) sizeof(pattern), "seed both sides of the CRC block boundary");
    CHECK(pwrite(fd, pattern, sizeof(pattern), bytes - sizeof(pattern)) ==
          (ssize_t) sizeof(pattern), "seed the exact file tail");
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    CHECK(crc32_file(path, &manifest.artifact_crc) == 0, "read legacy serial IEEE CRC oracle");
    CHECK(lseek(fd, 9, SEEK_SET) == 9, "set offset on the pinned checkpoint descriptor");
    CHECK(validate_page_checkpoint(fd, &manifest, path), "checkpoint CRC differs across blocks or at the tail");
    CHECK(lseek(fd, 0, SEEK_CUR) == 9, "the checksum moved the pinned descriptor's offset");

    CHECK(ftruncate(fd, 127) == 0, "prepare small serial checkpoint");
    manifest.artifact_bytes = 127;
    CHECK(crc32_file(path, &manifest.artifact_crc) == 0, "read serial oracle for the small file");
    CHECK(validate_page_checkpoint(fd, &manifest, path), "serial checkpoint CRC was rejected");
    manifest.artifact_crc ^= 1U;
    CHECK(!validate_page_checkpoint(fd, &manifest, path), "accepted a mismatched checkpoint CRC");
    manifest.artifact_crc ^= 1U;
    manifest.artifact_bytes = 128;
    CHECK(!validate_page_checkpoint(fd, &manifest, path), "accepted a truncated checkpoint");
    manifest.artifact_bytes = 126;
    CHECK(!validate_page_checkpoint(fd, &manifest, path), "accepted trailing checkpoint bytes");
    CHECK(fclose(file) == 0, "close checkpoint CRC fixture");
}

#endif

int
main(void)
{
    alarm(10);
    test_drop_during_two_pins();
    test_pinned_graph_is_not_reclaimable();
#ifdef CUVS_CAGRA_FOLD_ENABLED
    test_recovery_charges_pinned_previous_boot();
    test_recovery_keeps_current_boot_candidate();
    test_recovery_retires_only_previous_boot_history();
    test_page_drop_waits_for_legacy_export();
    test_checkpoint_crc_blocks();
#endif
    alarm(0);
    return 0;
}

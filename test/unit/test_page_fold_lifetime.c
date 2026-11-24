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
    CHECK(make_room(0, CUVS_RQ_LOAD, 8192, 0, &pick) != 0,
          "capacity picker counted pinned bytes as reclaimable");
    CHECK(!old.frees && !reclaimable.frees && g_page_graph_bytes[0] == 12288,
          "failed room pick freed a graph");
    CHECK(make_room(0, CUVS_RQ_LOAD, 4096, 0, &pick) == 0,
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

int
main(void)
{
    alarm(10);
    test_drop_during_two_pins();
    test_pinned_graph_is_not_reclaimable();
    alarm(0);
    return 0;
}

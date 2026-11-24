/* Standalone CPU test of the daemon wire handlers and graph ownership. */
#define main pg_cuvs_server_program_main
#define cuvs_cagra_free fold_test_free
#define cuvs_cagra_search fold_test_search
#include "../../src/pg_cuvs_server.c"
#undef cuvs_cagra_free
#undef cuvs_cagra_search
#undef main

#define CHECK(expr) \
    do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

typedef struct FoldHandle
{
    unsigned frees;
    bool freed_outside_registry;
    unsigned searches;
    int32_t nearest;
    float distance;
} FoldHandle;

void
fold_test_free(CuvsCagraIndex index, int device)
{
    FoldHandle *handle = index;
    int lock_result = pthread_mutex_trylock(&g_index_mutex);

    CHECK(device == 0 && handle && !handle->frees);
    ++handle->frees;
    CHECK(lock_result == 0 || lock_result == EBUSY);
    if (lock_result == 0)
    {
        handle->freed_outside_registry = true;
        pthread_mutex_unlock(&g_index_mutex);
    }
}

int
fold_test_search(CuvsCagraIndex index, const float *query, int dim, int k,
                 CuvsSearchResult *results, int device, uint32_t itopk, uint32_t width)
{
    FoldHandle *handle = index;

    (void) query;
    (void) itopk;
    (void) width;
    CHECK(device == 0 && dim == 8 && k > 0 && !handle->frees);
    ++handle->searches;
    for (int i = 0; i < k; ++i)
        results[i] = (CuvsSearchResult) {.item_id = -1};
    results[0] = (CuvsSearchResult) {.item_id = handle->nearest, .distance = handle->distance};
    return 0;
}

static PageCagraGraph *
make_parent(FoldHandle *handle)
{
    PageCagraGraph *parent = calloc(1, sizeof(*parent));

    CHECK(parent);
    *parent = (PageCagraGraph) {
        .system_identifier = 1, .db_oid = 2, .index_oid = 3, .index_nonce = 4,
        .base_nonce = 5, .graph_nonce = 5, .n_slots = 64, .dim = 8,
        .metric = CUVS_METRIC_L2, .handle = handle, .bytes = 100,
        .overlay = {.boot_id = 6, .generation = 1, .covered_upto = 4}
    };
    CHECK(!g_page_graphs && !g_n_page_graphs && !g_page_graph_bytes[0]);
    g_page_graphs = parent;
    g_n_page_graphs = 1;
    g_page_graph_bytes[0] = parent->bytes;
    return parent;
}

static CuvsPageFoldEvent
make_event(void)
{
    return (CuvsPageFoldEvent) {
        .length = sizeof(CuvsPageFoldEvent), .system_identifier = 1,
        .db_oid = 2, .index_oid = 3, .index_nonce = 4, .base_nonce = 5,
        .parent_graph_nonce = 5, .graph_nonce = 7, .parent_n_slots = 64,
        .boot_id = 6, .daemon_id = g_page_daemon_id, .positions = 4
    };
}

static uint32_t
ready(uint32_t db, uint64_t system, uint64_t boot, CuvsPageFoldEvent *event)
{
    CuvsCmdFrame cmd = {.op = CUVS_OP_FOLD_READY, .db_oid = db};
    CuvsPageFoldQuery request = {.length = sizeof(request), .system_identifier = system,
                                 .boot_id = boot};
    CuvsReplyHeader reply;
    int pair[2];

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(send_all(pair[0], &request, sizeof(request)) == 0);
    handle_page_fold_ready(pair[1], &cmd);
    close(pair[1]);
    CHECK(recv_all(pair[0], &reply, sizeof(reply)) == 0);
    if (reply.status == CUVS_STATUS_OK)
        CHECK(recv_all(pair[0], event, sizeof(*event)) == 0);
    CHECK(recv(pair[0], &request, 1, 0) == 0);
    close(pair[0]);
    return reply.status;
}

static uint32_t
fold_result(uint32_t op, uint32_t db, CuvsPageFoldEvent event, uint64_t lsn)
{
    CuvsCmdFrame cmd = {.op = op, .db_oid = db, .index_oid = event.index_oid};
    CuvsPageFoldResult request = {.event = event, .transition_lsn = lsn};
    CuvsReplyHeader reply;
    int pair[2];

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(send_all(pair[0], &request, sizeof(request)) == 0);
    handle_page_fold_result(pair[1], &cmd);
    close(pair[1]);
    CHECK(recv_all(pair[0], &reply, sizeof(reply)) == 0);
    CHECK(recv(pair[0], &request, 1, 0) == 0);
    close(pair[0]);
    release_retired_fold_graphs();
    release_retired_page_overlays();
    return reply.status;
}

static void
drop_index(void)
{
    pthread_mutex_lock(&g_index_mutex);
    drop_page_graphs(2, 3);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    release_retired_page_overlays();
}

static void
test_commit(void)
{
    FoldHandle old = {0}, child = {0};
    PageGraphFoldRef source = {0}, reader = {0};
    CuvsPageFoldEvent event = make_event(), received, wrong;
    PageCagraGraph *parent;
    CuvsPageOverlayRequest append = {.base.tail = 5, .boot_id = 6, .from = 4, .positions = 1};
    CuvsPageOverlayChunk *chunk, *obsolete, *head;

    pthread_mutex_lock(&g_index_mutex);
    parent = make_parent(&old);
    CHECK(page_graph_fold_pin_locked(parent, &source) == &old);
    wrong = event;
    ++wrong.positions;
    CHECK(!page_graph_register_fold_locked(&source, &wrong, &child, 80));
    CHECK(page_graph_register_fold_locked(&source, &event, &child, 80));
    CHECK(!page_graph_register_fold_locked(&source, &event, &child, 80));
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&source);

    CHECK(ready(3, 1, 6, &received) == CUVS_STATUS_NOT_FOUND);
    CHECK(ready(2, 9, 6, &received) == CUVS_STATUS_NOT_FOUND);
    CHECK(ready(2, 1, 9, &received) == CUVS_STATUS_NOT_FOUND);
    for (int i = 0; i < 2; ++i)
    {
        CHECK(ready(2, 1, 6, &received) == CUVS_STATUS_OK);
        CHECK(!memcmp(&event, &received, sizeof(event)));
    }

    pthread_mutex_lock(&g_index_mutex);
    CHECK(g_page_graphs == parent && g_n_page_graphs == 1 && g_page_graph_bytes[0] == 180);
    CHECK(!evict_page_graph(0));
    CHECK(page_graph_fold_pin_locked(parent, &reader) == &old);
    CHECK(cuvs_page_overlay_alloc(&g_page_overlay_budget, &append,
                                  sizeof(CuvsPagePosition), &chunk) == CUVS_STATUS_OK);
    chunk->marks[0] = (CuvsPagePosition) {.state = CUVS_PAGE_POSITION_DEAD};
    head = chunk;
    cuvs_page_overlay_retain(head);
    CHECK(cuvs_page_overlay_supply(&parent->overlay, &append, &chunk, &obsolete) == CUVS_STATUS_OK);
    CHECK(!chunk && !obsolete);
    pthread_mutex_unlock(&g_index_mutex);

    wrong = event;
    ++wrong.parent_n_slots;
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, wrong, 90) == CUVS_STATUS_NOT_FOUND);
    CHECK(fold_result(CUVS_OP_FOLD_DISCARD, 2, wrong, 0) == CUVS_STATUS_NOT_FOUND);
    wrong = event;
    wrong.daemon_id ^= 2;
    if (!wrong.daemon_id)
        wrong.daemon_id = 1;
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, wrong, 90) == CUVS_STATUS_NOT_FOUND);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 3, event, 90) == CUVS_STATUS_ERROR);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 0) == CUVS_STATUS_ERROR);
    CHECK(fold_result(CUVS_OP_FOLD_DISCARD, 2, event, 90) == CUVS_STATUS_ERROR);
    CHECK(g_page_graphs == parent && !old.frees && !child.frees);

    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 90) == CUVS_STATUS_OK);
    CHECK(g_page_graphs->handle == &child && g_n_page_graphs == 1);
    CHECK(g_page_graphs->base_nonce == 5 && g_page_graphs->graph_nonce == 7);
    CHECK(g_page_graphs->n_slots == 68 && g_page_graphs->folded == 4);
    CHECK(g_page_graphs->overlay.head == head && g_page_graphs->overlay.covered_upto == 5);
    CHECK(parent->fold_retired && !old.frees && g_page_graph_bytes[0] == 180);
    CHECK(ready(2, 1, 6, &received) == CUVS_STATUS_NOT_FOUND);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 90) == CUVS_STATUS_OK);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 91) == CUVS_STATUS_NOT_FOUND);
    CHECK(fold_result(CUVS_OP_FOLD_DISCARD, 2, event, 0) == CUVS_STATUS_NOT_FOUND);
    page_graph_fold_unpin(&reader);
    CHECK(old.frees == 1 && old.freed_outside_registry && g_page_graph_bytes[0] == 80);
    drop_index();
    CHECK(child.frees == 1 && !g_page_graph_bytes[0]);
    CHECK(g_page_overlay_budget.used != 0);
    cuvs_page_overlay_release(head);
    CHECK(!g_page_overlay_budget.used);
    puts("PASS READY redelivery, strict ACK, old graph pin and overlay handoff");
}

static void
test_discard_drop_and_boot(void)
{
    FoldHandle old = {0}, discarded = {0}, stale = {0}, dropped = {0};
    PageGraphFoldRef source = {0}, fresh = {0};
    CuvsPageFoldEvent event = make_event(), received;
    PageCagraGraph *parent;

    pthread_mutex_lock(&g_index_mutex);
    parent = make_parent(&old);
    CHECK(page_graph_fold_pin_locked(parent, &source) == &old);
    CHECK(page_graph_register_fold_locked(&source, &event, &discarded, 80));
    pthread_mutex_unlock(&g_index_mutex);
    CHECK(fold_result(CUVS_OP_FOLD_DISCARD, 2, event, 0) == CUVS_STATUS_OK);
    CHECK(discarded.frees == 1 && discarded.freed_outside_registry && g_page_graphs == parent);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 90) == CUVS_STATUS_NOT_FOUND);

    ++event.graph_nonce;
    pthread_mutex_lock(&g_index_mutex);
    CHECK(page_graph_register_fold_locked(&source, &event, &stale, 80));
    discard_page_fold_from_old_boot_locked(parent, 8);
    CHECK(!cuvs_page_overlay_start_boot(&parent->overlay, 8, 0));
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    CHECK(stale.frees == 1 && !old.frees && g_page_graph_bytes[0] == 100);
    CHECK(ready(2, 1, 6, &received) == CUVS_STATUS_NOT_FOUND);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 90) == CUVS_STATUS_NOT_FOUND);

    pthread_mutex_lock(&g_index_mutex);
    CHECK(!cuvs_page_overlay_start_boot(&parent->overlay, 6, 0));
    parent->overlay.covered_upto = 4;
    CHECK(!page_graph_register_fold_locked(&source, &event, &dropped, 80));
    CHECK(page_graph_fold_pin_locked(parent, &fresh) == &old);
    CHECK(page_graph_register_fold_locked(&fresh, &event, &dropped, 80));
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&fresh);
    drop_index();
    CHECK(dropped.frees == 1 && dropped.freed_outside_registry && !old.frees && !g_page_graphs);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 90) == CUVS_STATUS_NOT_FOUND);
    page_graph_fold_unpin(&source);
    CHECK(old.frees == 1 && !g_page_graph_bytes[0]);
    puts("PASS DISCARD, boot ABA and DROP invalidate the exact pending graph");
}

static uint32_t
page_search(bool overlay, CuvsPageOverlayRequest request, CuvsPageOverlayResult *rows, int *n)
{
    CuvsCmdFrame cmd = {.op = overlay ? CUVS_OP_SEARCH_PAGE_OVERLAY : CUVS_OP_SEARCH_PAGES,
                        .db_oid = 2, .index_oid = 3, .n_vecs = 1, .dim = 8, .k = 3};
    CuvsReplyHeader reply;
    CuvsPageProgress progress;
    float query[8] = {0};
    void *mapping;
    int pair[2], fd;

    snprintf(cmd.shm_key, sizeof(cmd.shm_key), "/cuvs-fe-%ld", (long) getpid());
    fd = shm_open(cmd.shm_key, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && ftruncate(fd, sizeof(query)) == 0);
    mapping = mmap(NULL, sizeof(query), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mapping != MAP_FAILED);
    memcpy(mapping, query, sizeof(query));
    CHECK(munmap(mapping, sizeof(query)) == 0);
    close(fd);
    g_peer_uid = getuid();
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    request.base.length = overlay ? sizeof(request) : sizeof(request.base);
    CHECK(send_all(pair[0], &request, request.base.length) == 0);
    if (overlay)
        handle_page_overlay_search(pair[1], &cmd);
    else
        handle_page_search(pair[1], &cmd);
    close(pair[1]);
    CHECK(shm_unlink(cmd.shm_key) == 0);
    CHECK(recv_all(pair[0], &reply, sizeof(reply)) == 0);
    if (reply.status == CUVS_STATUS_OK)
    {
        CHECK(reply.n_results <= cmd.k);
        if (overlay)
        {
            CHECK(recv_all(pair[0], &progress, sizeof(progress)) == 0);
            CHECK(recv_all(pair[0], rows, reply.n_results * sizeof(*rows)) == 0);
        }
        else
        {
            CuvsResult base[3];

            CHECK(recv_all(pair[0], base, reply.n_results * sizeof(*base)) == 0);
            for (uint32_t i = 0; i < reply.n_results; ++i)
                rows[i] = (CuvsPageOverlayResult) {.slot = base[i].tid, .distance = base[i].distance};
        }
    }
    *n = (int) reply.n_results;
    close(pair[0]);
    release_retired_fold_graphs();
    release_retired_page_overlays();
    return reply.status;
}

static void
test_search_before_and_after_ack(void)
{
    FoldHandle old = {.nearest = 10, .distance = 100}, child = {.nearest = 64};
    FoldHandle next_child = {.nearest = 64};
    PageGraphFoldRef pin = {0};
    PageCagraGraph *parent;
    CuvsPageFoldEvent event = make_event(), next, received;
    CuvsPageOverlayRequest supply = {.base.tail = 6, .boot_id = 6, .positions = 6};
    CuvsPageOverlayRequest search = {
        .base = {
            .system_identifier = 1, .index_nonce = 4, .base_nonce = 5,
            .graph_nonce = 5, .n_slots = 64, .tail = 6
        },
        .boot_id = 6, .from = 6
    };
    CuvsPageOverlayRequest graph_only;
    CuvsPageOverlayChunk *chunk, *obsolete;
    CuvsPageOverlayResult rows[3];
    const float vectors[3][8] = {{0}, {1}, {2}};
    int n;

    pthread_mutex_lock(&g_index_mutex);
    parent = make_parent(&old);
    parent->overlay.covered_upto = 0;
    CHECK(cuvs_page_overlay_alloc(&g_page_overlay_budget, &supply,
                                  6 * sizeof(*chunk->marks) + sizeof(vectors), &chunk) == CUVS_STATUS_OK);
    memset(chunk->marks, 0, 6 * sizeof(*chunk->marks));
    chunk->marks[0] = (CuvsPagePosition) {.block = 100, .state = CUVS_PAGE_POSITION_VECTOR};
    chunk->marks[4] = (CuvsPagePosition) {.block = 104, .state = CUVS_PAGE_POSITION_VECTOR};
    chunk->marks[5] = (CuvsPagePosition) {.block = 105, .state = CUVS_PAGE_POSITION_VECTOR};
    memcpy(chunk->marks + 6, vectors, sizeof(vectors));
    CHECK(cuvs_page_overlay_supply(&parent->overlay, &supply, &chunk, &obsolete) == CUVS_STATUS_OK);
    CHECK(!chunk && !obsolete);
    CHECK(page_graph_fold_pin_locked(parent, &pin) == &old);
    CHECK(page_graph_register_fold_locked(&pin, &event, &child, 80));
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&pin);

    CHECK(page_search(true, search, rows, &n) == CUVS_STATUS_OK);
    CHECK(n == 3 && rows[0].slot == 64 && rows[0].block == 100);
    search.base.graph_nonce = 7;
    search.base.n_slots = 68;
    search.base.folded = 4;
    graph_only = search;
    graph_only.base.tail = graph_only.base.folded;
    CHECK(page_search(false, graph_only, rows, &n) == CUVS_STATUS_NOT_FOUND);
    CHECK(page_search(true, search, rows, &n) == CUVS_STATUS_OK);
    CHECK(n == 3 && rows[0].slot == 64 && !rows[0].block);
    CHECK(rows[1].slot == 68 && rows[1].block == 104 && rows[1].distance == 1);
    CHECK(rows[2].slot == 69 && rows[2].block == 105 && rows[2].distance == 4);
    CHECK(old.searches == 1 && child.searches == 1 && !old.frees && g_page_graphs == parent);
    CHECK(fold_result(CUVS_OP_FOLD_DISCARD, 2, event, 0) == CUVS_STATUS_NOT_FOUND);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 90) == CUVS_STATUS_OK);
    CHECK(old.frees == 1 && child.frees == 0);
    CHECK(page_search(true, search, rows, &n) == CUVS_STATUS_OK);
    CHECK(n == 3 && rows[0].slot == 64 && rows[1].slot == 68 && rows[2].slot == 69);
    CHECK(page_search(false, graph_only, rows, &n) == CUVS_STATUS_OK);
    CHECK(n == 1 && rows[0].slot == 64);
    graph_only.base.base_nonce = graph_only.base.graph_nonce;
    CHECK(page_search(false, graph_only, rows, &n) == CUVS_STATUS_NOT_FOUND);

    next = event;
    next.parent_graph_nonce = event.graph_nonce;
    next.graph_nonce = 8;
    next.parent_n_slots = 68;
    next.parent_folded = 4;
    next.positions = 1;
    pthread_mutex_lock(&g_index_mutex);
    CHECK(page_graph_fold_pin_locked(g_page_graphs, &pin) == &child);
    CHECK(page_graph_register_fold_locked(&pin, &next, &next_child, 85));
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&pin);
    search.base.graph_nonce = next.graph_nonce;
    search.base.n_slots = 69;
    search.base.folded = 5;
    search.boot_id = 9;
    CHECK(page_search(true, search, rows, &n) == CUVS_STATUS_NOT_FOUND);
    CHECK(n == 0 && !next_child.searches && next_child.frees == 1 && !child.frees);
    CHECK(ready(2, 1, 6, &received) == CUVS_STATUS_NOT_FOUND);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, next, 91) == CUVS_STATUS_NOT_FOUND);
    pthread_mutex_lock(&g_index_mutex);
    CHECK(evict_page_graph(0));
    pthread_mutex_unlock(&g_index_mutex);
    CHECK(child.frees == 1 && !g_page_graph_bytes[0] && !g_page_overlay_budget.used);
    puts("PASS same-boot child search before ACK, folded ranges after ACK and new-boot cleanup");
}

static void
test_ready_visits_other_indexes(void)
{
    FoldHandle left = {0}, right = {0}, left_child = {0}, right_child = {0};
    FoldHandle newer = {0};
    PageGraphFoldRef left_pin = {0}, right_pin = {0}, next_pin = {0};
    CuvsPageFoldEvent first = make_event(), second = first, event;
    PageCagraGraph *parent, *other;
    uint32_t first_oid;

    pthread_mutex_lock(&g_index_mutex);
    parent = make_parent(&left);
    other = malloc(sizeof(*other));
    CHECK(other);
    *other = *parent;
    other->index_oid = 9;
    other->index_nonce = 14;
    other->base_nonce = other->graph_nonce = 15;
    other->handle = &right;
    other->next = parent;
    g_page_graphs = other;
    ++g_n_page_graphs;
    g_page_graph_bytes[0] += other->bytes;
    second.index_oid = other->index_oid;
    second.index_nonce = other->index_nonce;
    second.base_nonce = second.parent_graph_nonce = other->graph_nonce;
    second.graph_nonce = 17;

    CHECK(page_graph_fold_pin_locked(parent, &left_pin) == &left);
    CHECK(page_graph_fold_pin_locked(other, &right_pin) == &right);
    CHECK(page_graph_register_fold_locked(&left_pin, &first, &left_child, 80));
    CHECK(page_graph_register_fold_locked(&right_pin, &second, &right_child, 80));
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&left_pin);
    page_graph_fold_unpin(&right_pin);

    CHECK(ready(2, 1, 6, &event) == CUVS_STATUS_OK);
    first_oid = event.index_oid;
    CHECK(ready(2, 1, 6, &event) == CUVS_STATUS_OK);
    CHECK(event.index_oid != first_oid);
    CHECK(ready(2, 1, 6, &event) == CUVS_STATUS_OK);
    CHECK(event.index_oid == first_oid);
    CHECK(ready(2, 1, 6, &event) == CUVS_STATUS_OK);
    CHECK(event.index_oid != first_oid);
    CHECK(fold_result(CUVS_OP_FOLD_COMMITTED, 2, event, 90) == CUVS_STATUS_OK);

    /* The other index can finish and queue fresh work while the first
     * candidate is still outstanding. It must not overtake that candidate. */
    pthread_mutex_lock(&g_index_mutex);
    parent = g_page_graphs;
    while (parent && parent->index_oid != event.index_oid)
        parent = parent->next;
    CHECK(parent);
    parent->overlay.covered_upto = parent->folded + 1;
    event.parent_graph_nonce = parent->graph_nonce;
    event.graph_nonce = 18;
    event.parent_n_slots = parent->n_slots;
    event.parent_folded = parent->folded;
    event.positions = 1;
    CHECK(page_graph_fold_pin_locked(parent, &next_pin) == parent->handle);
    CHECK(page_graph_register_fold_locked(&next_pin, &event, &newer, 85));
    pthread_mutex_unlock(&g_index_mutex);
    page_graph_fold_unpin(&next_pin);
    CHECK(ready(2, 1, 6, &event) == CUVS_STATUS_OK);
    CHECK(event.index_oid == first_oid);

    pthread_mutex_lock(&g_index_mutex);
    drop_page_graphs(2, 9);
    pthread_mutex_unlock(&g_index_mutex);
    release_retired_fold_graphs();
    drop_index();
    CHECK(left.frees == 1 && right.frees == 1);
    CHECK(left_child.frees == 1 && right_child.frees == 1 && newer.frees == 1 &&
          !g_page_graph_bytes[0]);
    puts("PASS outstanding READY retains its candidate and does not hide another index");
}

int
main(void)
{
    alarm(10);
    (void) pthread_once(&g_page_daemon_once, initialize_page_daemon_id);
    CHECK(g_page_daemon_id);
    g_page_overlay_budget.limit = 4096;
    test_commit();
    test_discard_drop_and_boot();
    test_search_before_and_after_ack();
    test_ready_visits_other_indexes();
    return 0;
}

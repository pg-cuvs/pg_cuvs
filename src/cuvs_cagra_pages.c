#include "postgres.h"

#include "cuvs_cagra_pages.h"
#include "utils/memutils.h"

CuvsCagraRelCache *
cuvs_cagra_relation_cache(Relation index)
{
    CuvsCagraRelCache *cache = index->rd_amcache;

    if (cache == NULL)
    {
        cache = MemoryContextAllocZero(index->rd_indexcxt, sizeof(*cache));
        cache->page_format = cuvs_cagra_has_pages(index);
        index->rd_amcache = cache;
    }
    return cache;
}

void
cuvs_cagra_require_legacy(Relation index)
{
    if (cuvs_cagra_has_pages(index))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("this build cannot open the page-format CAGRA index \"%s\"",
                        RelationGetRelationName(index)),
                 errhint("Use a build that supports the index format or rebuild the index.")));
}

#ifdef CUVS_PAGE_FORMAT

#include "access/generic_xlog.h"
#include "access/genam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "cuvs_cagra_history.h"
#include "cuvs_util.h"
#ifdef CUVS_CAGRA_FOLD_ENABLED
#include "cuvs_page_recovery.h"
#endif
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "storage/bufpage.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

enum CuvsCagraPageKind
{
    CUVS_PAGE_TIDS = 1,
    CUVS_PAGE_PENDING = 2,
    CUVS_PAGE_PENDING_VECTOR = 4
};

typedef struct CuvsCagraTidPage
{
    uint16      kind;
    uint16      flags;
    uint16      dead;
    uint16      reserved;
    uint64      base_nonce;
    uint64      first_slot;
} CuvsCagraTidPage;

typedef struct CuvsCagraPending
{
    uint16      kind;
    uint16      flags;
    uint32      count;
    BlockNumber next;
    uint32      vector_dim;
    uint64      index_nonce;
    uint64      first_pos;
} CuvsCagraPending;

#ifdef CUVS_TEST_HOOKS
static bool cuvs_test_abort_staged_fold;
#endif

StaticAssertDecl(sizeof(ItemPointerData) == 6, "CAGRA TID size");
StaticAssertDecl(sizeof(CuvsCagraTidPage) == 24, "CAGRA TID page tag size");
StaticAssertDecl(sizeof(CuvsCagraPending) == 32, "CAGRA pending page tag size");
StaticAssertDecl(MAXALIGN(SizeOfPageHeaderData) + sizeof(CuvsCagraMeta) <= BLCKSZ,
                 "CAGRA metapage must fit");
StaticAssertDecl(MAXALIGN(SizeOfPageHeaderData) + sizeof(CuvsCagraTidPage) +
                 CUVS_CAGRA_TIDS_PER_PAGE * sizeof(ItemPointerData) <= BLCKSZ,
                 "CAGRA TID page must fit");
StaticAssertDecl(MAXALIGN(SizeOfPageHeaderData) + sizeof(CuvsCagraPending) +
                 CUVS_CAGRA_PENDING_PER_PAGE * sizeof(ItemPointerData) <= BLCKSZ,
                 "CAGRA pending page must fit");

bool
cuvs_cagra_can_store_pending_vector(int dim)
{
    /* Reserve the pgvector header's eight bytes when defining the layout limit. */
    const Size usable = BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - sizeof(CuvsCagraPending);

    return dim > 0 && (Size) dim <= (usable - sizeof(ItemPointerData) - 8) / sizeof(float);
}

static Size
pending_stride(uint32 vector_dim)
{
    return sizeof(ItemPointerData) + (Size) vector_dim * sizeof(float);
}

static uint32
pending_capacity(uint32 vector_dim)
{
    return (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - sizeof(CuvsCagraPending)) /
        pending_stride(vector_dim);
}

static uint32
pending_vector_dim(const CuvsCagraMeta *meta)
{
    return (meta->flags & CUVS_CAGRA_PENDING_VECTOR) ? meta->dim : 0;
}

uint64
cuvs_cagra_nonce(void)
{
    uint64 nonce;

    do
    {
        if (!pg_strong_random(&nonce, sizeof(nonce)))
            ereport(ERROR, (errmsg("could not generate CAGRA index identity")));
    } while (nonce == 0);
    return nonce;
}

static void
bad_page(Relation index, BlockNumber block)
{
    ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                    errmsg("invalid CAGRA index page at block %u of \"%s\"",
                           block, RelationGetRelationName(index)),
                    errhint("REINDEX INDEX %s using a build that supports the page format.",
                            quote_identifier(RelationGetRelationName(index)))));
}

static void
set_lower(Page page, Size bytes)
{
    ((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + bytes;
}

static CuvsCagraMeta *
page_meta(Page page)
{
    CuvsCagraMeta *meta;
    PageHeader header = (PageHeader) page;

    if (PageIsNew(page) || header->pd_lower < MAXALIGN(SizeOfPageHeaderData) + sizeof(*meta) ||
        header->pd_lower > header->pd_upper || header->pd_upper > BLCKSZ)
        return NULL;
    meta = (CuvsCagraMeta *) PageGetContents(page);
    if (meta->magic != CUVS_CAGRA_PAGE_MAGIC || meta->major != CUVS_CAGRA_PAGE_MAJOR ||
        meta->minor != 0 || (meta->flags & ~CUVS_CAGRA_PENDING_VECTOR) != 0 ||
        ((meta->flags & CUVS_CAGRA_PENDING_VECTOR) &&
         !cuvs_cagra_can_store_pending_vector(meta->dim)) ||
        meta->index_nonce == 0 || meta->base_nonce == 0 ||
        meta->n_extents > CUVS_CAGRA_PAGE_DIR ||
        meta->folded > meta->n_slots || meta->base_slots != meta->n_slots - meta->folded ||
        meta->n_slots > (uint64) meta->n_extents * CUVS_CAGRA_PAGE_EXTENT * CUVS_CAGRA_TIDS_PER_PAGE ||
        !BlockNumberIsValid(meta->pending_head) || meta->pending_head == 0 ||
        !BlockNumberIsValid(meta->pending_tail) || meta->pending_tail == 0)
        return NULL;
    return meta;
}

bool
cuvs_cagra_read_meta(Relation index, CuvsCagraMeta *meta)
{
    Buffer buf = ReadBuffer(index, 0);
    CuvsCagraMeta *found;

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    found = page_meta(BufferGetPage(buf));
    if (found != NULL)
        memcpy(meta, found, sizeof(*meta));
    UnlockReleaseBuffer(buf);
    return found != NULL;
}

static Buffer
extend_page(Relation index, ForkNumber fork)
{
    return ExtendBufferedRel(BMR_REL(index), fork, NULL,
                             EB_LOCK_FIRST | EB_CREATE_FORK_IF_NEEDED);
}

static Page
new_page(GenericXLogState *wal, Buffer buf, Size special)
{
    Page page = GenericXLogRegisterBuffer(wal, buf, GENERIC_XLOG_FULL_IMAGE);

    PageInit(page, BLCKSZ, special);
    return page;
}

static void
finish_page(GenericXLogState *wal, Buffer buf, ForkNumber fork)
{
    GenericXLogFinish(wal);
    if (fork == INIT_FORKNUM)
    {
        /* Init forks are copied after a crash, even for unlogged relations. */
        log_newpage_buffer(buf, true);
        FlushOneBuffer(buf);
    }
    UnlockReleaseBuffer(buf);
}

static CuvsCagraPending *
init_pending(Page page, uint64 nonce, uint64 first_pos, uint32 vector_dim)
{
    CuvsCagraPending *pending = (CuvsCagraPending *) PageGetSpecialPointer(page);

    pending->kind = vector_dim ? CUVS_PAGE_PENDING_VECTOR : CUVS_PAGE_PENDING;
    pending->vector_dim = vector_dim;
    pending->index_nonce = nonce;
    pending->first_pos = first_pos;
    pending->next = InvalidBlockNumber;
    return pending;
}

void
cuvs_cagra_init_pages(Relation index, ForkNumber fork, uint64 index_nonce,
                      uint64 graph_nonce, int dim, uint32 metric,
                      const uint64 *tids, int64 n, bool pending_vectors)
{
    CuvsCagraMeta meta = {0};
    Buffer mb;
    Buffer buf;
    GenericXLogState *wal;
    Page page;
    CuvsCagraFoldPage *fold_page;
    CuvsCagraFoldRecord *record;
    int64 pos = 0;

    if (n < 0 || (n > 0 && tids == NULL) ||
        n > (int64) CUVS_CAGRA_PAGE_DIR * CUVS_CAGRA_PAGE_EXTENT * CUVS_CAGRA_TIDS_PER_PAGE)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("CAGRA build exceeds the page directory capacity")));
    meta.magic = CUVS_CAGRA_PAGE_MAGIC;
    meta.major = CUVS_CAGRA_PAGE_MAJOR;
    meta.flags = pending_vectors ? CUVS_CAGRA_PENDING_VECTOR : 0;
    meta.dim = dim;
    meta.metric = metric;
    meta.index_nonce = index_nonce;
    meta.graph_nonce = graph_nonce;
    meta.base_nonce = graph_nonce != 0 ? graph_nonce : cuvs_cagra_nonce();
    meta.n_slots = n;
    meta.base_slots = n;
    meta.extent_reclaim_from = InvalidBlockNumber;
    meta.foldlog_spare = InvalidBlockNumber;

    mb = extend_page(index, fork);
    if (BufferGetBlockNumber(mb) != 0)
        bad_page(index, BufferGetBlockNumber(mb));
    while (pos < n)
    {
        BlockNumber first = InvalidBlockNumber;

        for (int i = 0; i < CUVS_CAGRA_PAGE_EXTENT; ++i)
        {
            uint32 count = Min((int64) CUVS_CAGRA_TIDS_PER_PAGE, n - pos);
            BlockNumber block;
            CuvsCagraTidPage *tag;
            ItemPointerData *items;

            buf = extend_page(index, fork);
            block = BufferGetBlockNumber(buf);
            if (i == 0)
            {
                first = block;
                meta.extents[meta.n_extents++] = first;
            }
            else if (block != first + i)
                bad_page(index, block);

            wal = GenericXLogStart(index);
            page = new_page(wal, buf, sizeof(*tag));
            tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
            tag->kind = CUVS_PAGE_TIDS;
            tag->base_nonce = meta.base_nonce;
            tag->first_slot = (uint64) (meta.n_extents - 1) *
                CUVS_CAGRA_PAGE_EXTENT * CUVS_CAGRA_TIDS_PER_PAGE +
                i * CUVS_CAGRA_TIDS_PER_PAGE;
            items = (ItemPointerData *) PageGetContents(page);
            for (uint32 j = 0; j < count; ++j)
            {
                uint32 blockno;
                uint16 offset;

                cuvs_tid_decode(tids[pos + j], &blockno, &offset);
                ItemPointerSet(&items[j], blockno, offset);
            }
            pos += count;
            set_lower(page, count * sizeof(*items));
            finish_page(wal, buf, fork);
        }
    }

    buf = extend_page(index, fork);
    meta.pending_head = meta.pending_tail = BufferGetBlockNumber(buf);
    wal = GenericXLogStart(index);
    page = new_page(wal, buf, sizeof(CuvsCagraPending));
    init_pending(page, index_nonce, 0, pending_vector_dim(&meta));
    finish_page(wal, buf, fork);

    buf = extend_page(index, fork);
    meta.foldlog_head = meta.foldlog_tail = BufferGetBlockNumber(buf);
    wal = GenericXLogStart(index);
    page = new_page(wal, buf, sizeof(CuvsCagraFoldPage));
    fold_page = (CuvsCagraFoldPage *) PageGetSpecialPointer(page);
    fold_page->kind = CUVS_PAGE_FOLDLOG;
    fold_page->next = InvalidBlockNumber;
    if (graph_nonce != 0)
    {
        record = (CuvsCagraFoldRecord *) PageGetContents(page);
        record->graph_nonce = graph_nonce;
        record->n_slots = n;
        record->folded = 0;
        fold_page->count = 1;
        set_lower(page, sizeof(*record));
    }
    finish_page(wal, buf, fork);

    wal = GenericXLogStart(index);
    page = new_page(wal, mb, 0);
    memcpy(PageGetContents(page), &meta, sizeof(meta));
    set_lower(page, sizeof(meta));
    finish_page(wal, mb, fork);
}

void
cuvs_cagra_check_dim(int expected, int actual)
{
    if (expected != 0 && expected != actual)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                        errmsg("expected %d dimensions, not %d", expected, actual),
                        errhint("Use a vector column with a fixed dimension.")));
}

void
cuvs_cagra_check_query_dim(Relation index, int dim)
{
    CuvsCagraMeta meta;

    if (!cuvs_cagra_read_meta(index, &meta))
        ereport(WARNING, (errmsg("unrecognized CAGRA metapage for index \"%s\"",
                                 RelationGetRelationName(index)),
                          errdetail("Using an exact heap scan.")));
    else if (dim != 0)
        cuvs_cagra_check_dim(meta.dim, dim);
}

static CuvsCagraPending *
pending_tag(Page page, BlockNumber block, uint64 nonce, uint32 vector_dim)
{
    CuvsCagraPending *pending;
    PageHeader header = (PageHeader) page;

    if (PageIsNew(page) || PageGetSpecialSize(page) != sizeof(*pending))
        return NULL;
    pending = (CuvsCagraPending *) PageGetSpecialPointer(page);
    if (pending->kind != (vector_dim ? CUVS_PAGE_PENDING_VECTOR : CUVS_PAGE_PENDING) ||
        pending->index_nonce != nonce || pending->vector_dim != vector_dim ||
        pending->flags != 0 || pending->count > pending_capacity(vector_dim) ||
        header->pd_lower != MAXALIGN(SizeOfPageHeaderData) + pending->count * pending_stride(vector_dim) ||
        pending->next == block || pending->next == 0)
        return NULL;
    return pending;
}

static CuvsCagraPending *
page_pending(Relation index, BlockNumber block, Page page, uint64 nonce, uint32 vector_dim)
{
    CuvsCagraPending *pending = pending_tag(page, block, nonce, vector_dim);

    if (!pending)
        bad_page(index, block);
    return pending;
}

bool
cuvs_cagra_read_tail(Relation index, const CuvsCagraMeta *meta,
                     BlockNumber nblocks, uint64 *tail)
{
    Buffer buf;
    const CuvsCagraPending *tag;
    bool valid;

    if (meta->pending_tail >= nblocks)
        return false;
    buf = ReadBuffer(index, meta->pending_tail);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    tag = pending_tag(BufferGetPage(buf), meta->pending_tail, meta->index_nonce,
                      pending_vector_dim(meta));
    valid = tag && !BlockNumberIsValid(tag->next) &&
        tag->first_pos <= UINT64_MAX - tag->count;
    if (valid)
        *tail = tag->first_pos + tag->count;
    UnlockReleaseBuffer(buf);
    return valid;
}

static bool
copy_pending_positions(Relation index, const CuvsCagraMeta *meta, BlockNumber nblocks,
                       uint64 from, uint64 upto, CuvsCagraPendingLocation *locations,
                       float *vectors)
{
    BlockNumber block = meta->pending_tail;
    bool seek_tail = true;
    bool first_page = true;
    uint32 vector_dim = pending_vector_dim(meta);
    Size stride = pending_stride(vector_dim);
    uint64 position = from;
    uint64 next_position = 0;

    if (from > upto)
        return false;
    while (position < upto)
    {
        Buffer buf;
        Page page;
        const CuvsCagraPending *tag;
        BlockNumber next;
        uint64 end;

        CHECK_FOR_INTERRUPTS();
        if (block == 0 || block >= nblocks)
            return false;
        buf = ReadBuffer(index, block);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        tag = pending_tag(page, block, meta->index_nonce, vector_dim);
        if (!tag || tag->first_pos > UINT64_MAX - tag->count ||
            (!first_page && tag->first_pos != next_position))
        {
            UnlockReleaseBuffer(buf);
            return false;
        }
        if (seek_tail && position < tag->first_pos)
        {
            UnlockReleaseBuffer(buf);
            block = meta->pending_head;
            seek_tail = false;
            first_page = true;
            continue;
        }
        if (position < tag->first_pos)
        {
            UnlockReleaseBuffer(buf);
            return false;
        }

        end = tag->first_pos + tag->count;
        next = tag->next;
        for (; position < Min(end, upto); ++position)
        {
            CuvsCagraPendingLocation *item = &locations[position - from];
            const char *entry = (char *) PageGetContents(page) +
                (position - tag->first_pos) * stride;

            item->block = block;
            memcpy(&item->tid, entry, sizeof(item->tid));
            if (vector_dim && vectors && ItemPointerIsValid(&item->tid))
                memcpy(vectors + (position - from) * vector_dim,
                       entry + sizeof(item->tid), (Size) vector_dim * sizeof(float));
        }
        seek_tail = false;
        first_page = false;
        if (position < upto && (!BlockNumberIsValid(next) || !tag->count))
        {
            UnlockReleaseBuffer(buf);
            return false;
        }
        UnlockReleaseBuffer(buf);
        next_position = end;
        block = next;
    }
    return true;
}

static bool read_slots(Relation index, const CuvsCagraMeta *meta,
                       BlockNumber nblocks, uint64 slot, uint32 count, ItemPointer tids);

bool
cuvs_cagra_copy_pending(Relation index, const CuvsCagraMeta *meta, BlockNumber nblocks,
                        uint64 from, uint64 upto, CuvsCagraPendingLocation *locations,
                        float *vectors)
{
    CuvsCagraMeta current;
    bool valid = false;
    uint64 end;

    if (!cuvs_page_resident_search)
        return from >= meta->folded &&
            copy_pending_positions(index, meta, nblocks, from, upto, locations, vectors);

    /* Fold and VACUUM may have moved a position to the slot directory since
     * the scan read its metadata. Read both maps under the same fence. */
    LockPage(index, 0, ShareLock);
    if (!cuvs_cagra_read_meta(index, &current) || current.index_nonce != meta->index_nonce ||
        current.base_nonce != meta->base_nonce || current.base_slots != meta->base_slots ||
        current.folded < meta->folded || current.dim != meta->dim || current.flags != meta->flags)
        goto done;

    nblocks = RelationGetNumberOfBlocks(index);
    if (!copy_pending_positions(index, &current, nblocks, from, upto, locations, vectors))
        goto done;

    end = Min(upto, current.folded);
    for (uint64 position = from; position < end;)
    {
        ItemPointerData tids[CUVS_CAGRA_TIDS_PER_PAGE];
        uint64 slot = current.base_slots + position;
        uint32 count = Min(end - position,
                           CUVS_CAGRA_TIDS_PER_PAGE - slot % CUVS_CAGRA_TIDS_PER_PAGE);

        CHECK_FOR_INTERRUPTS();
        if (!read_slots(index, &current, nblocks, slot, count, tids))
            goto done;
        for (uint32 i = 0; i < count; ++i)
        {
            ItemPointer root = &locations[position - from + i].tid;

            /* The pending vector is still usable only for its original root.
             * VACUUM clears the canonical slot and leaves the old bytes. */
            if (!ItemPointerIsValid(&tids[i]))
                ItemPointerSetInvalid(root);
            else if (!ItemPointerIsValid(root) || !ItemPointerEquals(root, &tids[i]))
                goto done;
        }
        position += count;
    }
    valid = true;

done:
    UnlockPage(index, 0, ShareLock);
    return valid;
}

static bool
fold_base_matches(const CuvsCagraMeta *meta, const CuvsCagraFoldCandidate *candidate)
{
    return meta->index_nonce == candidate->index_nonce &&
        meta->base_nonce == candidate->base_nonce &&
        meta->graph_nonce == candidate->parent_graph_nonce &&
        meta->n_slots == candidate->parent_n_slots &&
        meta->folded == candidate->parent_folded;
}

static bool
fold_candidate_valid(const CuvsCagraFoldCandidate *candidate)
{
    const uint64 capacity = (uint64) CUVS_CAGRA_PAGE_DIR *
        CUVS_CAGRA_PAGE_EXTENT * CUVS_CAGRA_TIDS_PER_PAGE;

    return candidate->positions != 0 && candidate->graph_nonce != 0 &&
        candidate->parent_graph_nonce != 0 &&
        candidate->graph_nonce != candidate->parent_graph_nonce &&
        candidate->parent_n_slots <= capacity &&
        candidate->positions <= capacity - candidate->parent_n_slots &&
        candidate->parent_folded <= candidate->parent_n_slots;
}

CuvsCagraFoldStatus
cuvs_cagra_prepare_fold(Relation index, const CuvsCagraFoldCandidate *candidate,
                        CuvsCagraMeta *base, CuvsCagraPendingLocation *locations)
{
    CuvsCagraMeta meta;
    BlockNumber blocks;

    if (!fold_candidate_valid(candidate))
        return CUVS_CAGRA_FOLD_DISCARD;

    /* Use the same heavyweight fence as VACUUM. No metapage buffer lock is
     * held while taking locks on the pending pages. */
    if (!ConditionalLockPage(index, 0, ExclusiveLock))
        return CUVS_CAGRA_FOLD_DEFERRED;

    blocks = RelationGetNumberOfBlocks(index);
    if (blocks == 0 || !cuvs_cagra_read_meta(index, &meta) ||
        !fold_base_matches(&meta, candidate) ||
        !copy_pending_positions(index, &meta, RelationGetNumberOfBlocks(index), meta.folded,
                                meta.folded + candidate->positions, locations, NULL))
    {
        UnlockPage(index, 0, ExclusiveLock);
        return CUVS_CAGRA_FOLD_DISCARD;
    }
    *base = meta;
    return CUVS_CAGRA_FOLD_PREPARED;
}

static int
fold_block_cmp(const void *a, const void *b)
{
    BlockNumber left = *(const BlockNumber *) a;
    BlockNumber right = *(const BlockNumber *) b;

    return (left > right) - (left < right);
}

/* The FCL excludes other folds and VACUUM. Pending appends allocate at EOF
 * with the new buffer exclusively locked until initialized, so a shared
 * buffer lock sees either a pending page or a page left by an aborted append.
 * Never use any part of an extent already in the metapage directory, even
 * where it has no slots yet. */
static BlockNumber
fold_find_orphan_extent(Relation index, uint64 base_nonce, BlockNumber nblocks,
                        const BlockNumber *published, uint32 n_published,
                        BlockNumber *next, uint32 *extent, BufferAccessStrategy strategy)
{
    uint32 run = 0;

    while (*next < nblocks)
    {
        BlockNumber block = (*next)++;
        Buffer buffer;
        Page page;
        bool reusable;

        CHECK_FOR_INTERRUPTS();
        while (*extent < n_published && published[*extent] + CUVS_CAGRA_PAGE_EXTENT <= block)
            ++*extent;
        if (*extent < n_published && block >= published[*extent])
        {
            *next = published[*extent] + CUVS_CAGRA_PAGE_EXTENT;
            ++*extent;
            run = 0;
            continue;
        }

        buffer = ReadBufferExtended(index, MAIN_FORKNUM, block, RBM_NORMAL, strategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);
        reusable = PageIsNew(page);
        if (!reusable && PageGetSpecialSize(page) == sizeof(CuvsCagraTidPage))
        {
            const CuvsCagraTidPage *tag = (const CuvsCagraTidPage *) PageGetSpecialPointer(page);

            reusable = tag->kind == CUVS_PAGE_TIDS && tag->flags == 0 &&
                tag->base_nonce == base_nonce;
        }
        UnlockReleaseBuffer(buffer);
        run = reusable ? run + 1 : 0;
        if (run == CUVS_CAGRA_PAGE_EXTENT)
            return *next - run;
    }
    /* Future appends or reservations may complete the last partial run. */
    *next -= run;
    return InvalidBlockNumber;
}

static BlockNumber
fold_reserve_extent(Relation index, const CuvsCagraMeta *directory, BlockNumber first)
{
    const uint64 first_slot = (uint64) directory->n_extents * CUVS_CAGRA_PAGE_EXTENT *
        CUVS_CAGRA_TIDS_PER_PAGE;
    bool reuse = BlockNumberIsValid(first);

    if (!reuse)
    {
        Buffer buffers[CUVS_CAGRA_PAGE_EXTENT];
        uint32 done = 0;

        /* Concurrent appends can extend the relation too. */
        LockRelationForExtension(index, ExclusiveLock);
        while (done < CUVS_CAGRA_PAGE_EXTENT)
        {
            uint32 extended;
            BlockNumber start = ExtendBufferedRelBy(BMR_REL(index), MAIN_FORKNUM, NULL,
                EB_SKIP_EXTENSION_LOCK, CUVS_CAGRA_PAGE_EXTENT - done, buffers, &extended);

            if (done == 0)
                first = start;
            else if (start != first + done)
                bad_page(index, start);
            for (uint32 i = 0; i < extended; ++i)
                ReleaseBuffer(buffers[i]);
            done += extended;
        }
        UnlockRelationForExtension(index, ExclusiveLock);
    }

    /* Reset stamped pages when adopting a range. Always log the final page
     * so redo has the full extent even when publication fills only part of it. */
    for (uint32 i = reuse ? 0 : CUVS_CAGRA_PAGE_EXTENT - 1; i < CUVS_CAGRA_PAGE_EXTENT; ++i)
    {
        Buffer buffer = ReadBuffer(index, first + i);
        GenericXLogState *state;
        Page page;
        CuvsCagraTidPage *tag;

        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        if (i < CUVS_CAGRA_PAGE_EXTENT - 1 && PageIsNew(BufferGetPage(buffer)))
        {
            UnlockReleaseBuffer(buffer);
            continue;
        }
        state = GenericXLogStart(index);
        page = new_page(state, buffer, sizeof(*tag));
        tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
        tag->kind = CUVS_PAGE_TIDS;
        tag->base_nonce = directory->base_nonce;
        tag->first_slot = first_slot + (uint64) i * CUVS_CAGRA_TIDS_PER_PAGE;
        set_lower(page, 0);
        finish_page(state, buffer, MAIN_FORKNUM);
    }
    return first;
}

static uint32
fold_write_slots(Page page, uint32 offset, uint32 count,
                 const CuvsCagraPendingLocation *locations)
{
    ItemPointerData *items = (ItemPointerData *) PageGetContents(page) + offset;
    CuvsCagraTidPage *tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
    uint32 dead = 0;

    for (uint32 i = 0; i < count; ++i)
    {
        items[i] = locations[i].tid;
        if (!ItemPointerIsValid(&items[i]))
            ++dead;
    }
    tag->dead += dead;
    set_lower(page, (offset + count) * sizeof(*items));
    return dead;
}

static BlockNumber
fold_slot_block(Relation index, const CuvsCagraMeta *directory,
                BlockNumber nblocks, uint64 slot)
{
    uint64 page_no = slot / CUVS_CAGRA_TIDS_PER_PAGE;
    uint32 extent = page_no / CUVS_CAGRA_PAGE_EXTENT;
    BlockNumber first;

    if (extent >= directory->n_extents)
        bad_page(index, 0);
    first = directory->extents[extent];
    if (first == 0 || first >= nblocks || nblocks - first < CUVS_CAGRA_PAGE_EXTENT)
        bad_page(index, 0);
    return first + page_no % CUVS_CAGRA_PAGE_EXTENT;
}

/* These pages contain no old published slots, so their page tags and TIDs
 * can be written ahead of the metapage. A partial old page is handled in
 * the transition record because VACUUM also reads its dead count. */
static uint64
fold_stage_tid_pages(Relation index, const CuvsCagraMeta *directory,
                     BlockNumber nblocks, uint64 slot, uint64 end,
                     const CuvsCagraPendingLocation *locations)
{
    uint64 dead = 0;

    while (slot < end)
    {
        Buffer buffers[MAX_GENERIC_XLOG_PAGES];
        int count = 0;
        GenericXLogState *state = GenericXLogStart(index);

        CHECK_FOR_INTERRUPTS();
        while (slot < end && count < lengthof(buffers))
        {
            uint32 positions = Min((uint64) CUVS_CAGRA_TIDS_PER_PAGE, end - slot);
            BlockNumber block = fold_slot_block(index, directory, nblocks, slot);
            Buffer buffer;
            Page page;
            CuvsCagraTidPage *tag;

            for (int i = 0; i < count; ++i)
                if (BufferGetBlockNumber(buffers[i]) == block)
                    bad_page(index, block);
            buffer = ReadBuffer(index, block);
            LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
            buffers[count++] = buffer;
            page = BufferGetPage(buffer);
            /* A previous fold may have reserved this block without using it. */
            if (!PageIsNew(page))
            {
                if (PageGetSpecialSize(page) != sizeof(*tag))
                    bad_page(index, block);
                tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
                if (tag->kind != CUVS_PAGE_TIDS || tag->flags != 0 ||
                    tag->base_nonce != directory->base_nonce || tag->first_slot != slot)
                    bad_page(index, block);
            }
            page = new_page(state, buffer, sizeof(*tag));
            tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
            tag->kind = CUVS_PAGE_TIDS;
            tag->base_nonce = directory->base_nonce;
            tag->first_slot = slot;
            dead += fold_write_slots(page, 0, positions, locations);
            slot += positions;
            locations += positions;
        }
        GenericXLogFinish(state);
        while (count)
            UnlockReleaseBuffer(buffers[--count]);
    }
    return dead;
}

static CuvsCagraFoldPage *
fold_log_tail(Relation index, const CuvsCagraMeta *meta, Page page)
{
    CuvsCagraFoldPage *tag;
    CuvsCagraFoldRecord *record;

    if (PageIsNew(page) || PageGetSpecialSize(page) != MAXALIGN(sizeof(*tag)))
        bad_page(index, meta->foldlog_tail);
    tag = (CuvsCagraFoldPage *) PageGetSpecialPointer(page);
    if (tag->kind != CUVS_PAGE_FOLDLOG || tag->flags != 0 ||
        tag->count == 0 || tag->count > CUVS_CAGRA_FOLD_RECORDS_PER_PAGE ||
        BlockNumberIsValid(tag->next) ||
        ((PageHeader) page)->pd_lower != MAXALIGN(SizeOfPageHeaderData) +
            tag->count * sizeof(*record) ||
        ((PageHeader) page)->pd_lower > ((PageHeader) page)->pd_upper)
        bad_page(index, meta->foldlog_tail);
    record = (CuvsCagraFoldRecord *) PageGetContents(page) + tag->count - 1;
    if (record->graph_nonce != meta->graph_nonce || record->n_slots != meta->n_slots ||
        record->folded != meta->folded)
        bad_page(index, meta->foldlog_tail);
    return tag;
}

#ifdef CUVS_CAGRA_FOLD_ENABLED
/* Copy one PG-authoritative lineage while publication is excluded. No IPC
 * or heap scan runs under FCL. Any malformed or changing chain is rejected. */
CuvsPageRecoveryHead *
cuvs_cagra_read_lineage(Relation index, const CuvsCagraMeta *expected, uint32 *count, XLogRecPtr *lsn)
{
    CuvsCagraMeta meta;
    CuvsPageRecoveryHead *heads = NULL;
    uint32 capacity = 0;
    uint32 used = 0;
    BlockNumber block, blocks;
    bool valid = false;
    CuvsPageRecoveryHead target = {
        .graph_nonce = expected->graph_nonce, .n_slots = expected->n_slots, .folded = expected->folded,
    };

    *count = 0;
    if (!ConditionalLockPage(index, 0, ShareLock))
        return NULL;
    Buffer metapage = ReadBuffer(index, 0);
    LockBuffer(metapage, BUFFER_LOCK_SHARE);
    Page image = BufferGetPage(metapage);
    const CuvsCagraMeta *current = page_meta(image);

    if (current)
        meta = *current;
    *lsn = PageGetLSN(image);
    UnlockReleaseBuffer(metapage);
    if (!current || meta.index_nonce != expected->index_nonce ||
        meta.base_nonce != expected->base_nonce || meta.graph_nonce != expected->graph_nonce ||
        meta.n_slots != expected->n_slots || meta.folded != expected->folded ||
        XLogRecPtrIsInvalid(*lsn))
        goto done;

    blocks = RelationGetNumberOfBlocks(index);
    if (!meta.foldlog_head || meta.foldlog_head > meta.foldlog_tail || meta.foldlog_tail >= blocks)
        goto done;
    block = meta.foldlog_head;
    for (;;)
    {
        CuvsCagraFoldPage tag;
        CuvsCagraFoldRecord records[CUVS_CAGRA_FOLD_RECORDS_PER_PAGE];

        CHECK_FOR_INTERRUPTS();
        if (!cuvs_cagra_history_read_fold_log_page(index, block, &tag, records))
            break;

        for (uint32 i = 0; i < tag.count; ++i)
        {
            CuvsPageRecoveryHead head = {
                .graph_nonce = records[i].graph_nonce, .n_slots = records[i].n_slots,
                .folded = records[i].folded,
            };

            if ((!used && (head.graph_nonce != meta.base_nonce ||
                           head.n_slots != meta.base_slots || head.folded)) ||
                !cuvs_page_recovery_lineage(&head, &target) ||
                (used && (head.n_slots <= heads[used - 1].n_slots ||
                          !cuvs_page_recovery_lineage(&heads[used - 1], &head))))
            {
                goto done;
            }
            if (used == capacity)
            {
                capacity = capacity ? capacity * 2 : CUVS_PAGE_RECOVERY_BATCH;
                if (!AllocSizeIsValid((Size) capacity * sizeof(*heads)))
                    goto done;
                heads = heads ? repalloc(heads, (Size) capacity * sizeof(*heads))
                              : palloc((Size) capacity * sizeof(*heads));
            }
            heads[used++] = head;
        }
        if (block == meta.foldlog_tail)
        {
            valid = !BlockNumberIsValid(tag.next) && cuvs_page_recovery_head_matches(&heads[used - 1], &target);
            break;
        }
        if (tag.count != CUVS_CAGRA_FOLD_RECORDS_PER_PAGE || tag.next <= block || tag.next > meta.foldlog_tail)
            break;
        block = tag.next;
    }
done:
    UnlockPage(index, 0, ShareLock);
    if (!valid)
    {
        if (heads)
            pfree(heads);
        return NULL;
    }
    *count = used;
    return heads;
}
#endif

/* The event is retained by the daemon until acknowledged. A replacement
 * worker can prove the same publication from the current metapage and log. */
static CuvsCagraFoldStatus
fold_retry_ack(Relation index, const CuvsCagraFoldCandidate *candidate, XLogRecPtr *commit_lsn)
{
    CuvsCagraMeta meta;
    XLogRecPtr lsn = InvalidXLogRecPtr;
    bool have_meta;
    bool same_lineage;
    bool uncertain;

    if (!ConditionalLockPage(index, 0, ShareLock))
        return CUVS_CAGRA_FOLD_DEFERRED;
    have_meta = cuvs_cagra_has_pages(index) && cuvs_cagra_read_meta(index, &meta);
    same_lineage = have_meta && meta.index_nonce == candidate->index_nonce &&
        meta.base_nonce == candidate->base_nonce;
    uncertain = !have_meta || (same_lineage && !fold_base_matches(&meta, candidate));
    if (same_lineage && meta.graph_nonce == candidate->graph_nonce &&
        meta.n_slots == candidate->parent_n_slots + candidate->positions &&
        meta.folded == candidate->parent_folded + candidate->positions)
    {
        Buffer buffer;
        Page page;

        if (!BlockNumberIsValid(meta.foldlog_tail) || meta.foldlog_tail == 0 ||
            meta.foldlog_tail >= RelationGetNumberOfBlocks(index))
            bad_page(index, 0);
        buffer = ReadBuffer(index, meta.foldlog_tail);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);
        (void) fold_log_tail(index, &meta, page);
        lsn = PageGetLSN(page);
        if (XLogRecPtrIsInvalid(lsn))
            bad_page(index, meta.foldlog_tail);
        UnlockReleaseBuffer(buffer);
    }
    UnlockPage(index, 0, ShareLock);
    if (XLogRecPtrIsInvalid(lsn))
    {
        /* A concurrent worker can present an event whose child was already
         * published and superseded. Never discard an uncertain publication. */
        if (cuvs_page_resident_search && uncertain)
            return CUVS_CAGRA_FOLD_DEFERRED;
        return CUVS_CAGRA_FOLD_DISCARD;
    }
    XLogFlush(lsn);
    *commit_lsn = lsn;
    return CUVS_CAGRA_FOLD_PUBLISHED;
}

static CuvsCagraFoldStatus
publish_fold(Relation index, const CuvsCagraFoldCandidate *candidate, XLogRecPtr *commit_lsn)
{
    const uint64 extent_slots = (uint64) CUVS_CAGRA_PAGE_EXTENT * CUVS_CAGRA_TIDS_PER_PAGE;
    CuvsCagraPendingLocation *locations;
    CuvsCagraMeta directory;
    CuvsCagraMeta *meta;
    CuvsCagraFoldStatus status;
    uint64 end, dead, xid = 0;
    BlockNumber nblocks, partial_block = InvalidBlockNumber;
    uint32 old_extents;
    uint32 offset, partial;
    bool rollover;
    TimestampTz finished;
    Buffer tid_buffer = InvalidBuffer;
    Buffer log_buffer, next_log = InvalidBuffer, meta_buffer;
    Page page;
    CuvsCagraFoldPage *tag;
    CuvsCagraFoldRecord *record;
    GenericXLogState *state;
    XLogRecPtr lsn;

    *commit_lsn = InvalidXLogRecPtr;
    if (!RelationNeedsWAL(index) || candidate->positions == 0 ||
        candidate->positions > MaxAllocSize / sizeof(*locations))
        return CUVS_CAGRA_FOLD_DISCARD;
    locations = palloc((Size) candidate->positions * sizeof(*locations));
    status = cuvs_cagra_prepare_fold(index, candidate, &directory, locations);
    if (status != CUVS_CAGRA_FOLD_PREPARED)
    {
        pfree(locations);
        return status;
    }

    if (cuvs_page_resident_search)
    {
        FullTransactionId transition = GetTopFullTransactionId();

        if (!cuvs_cagra_history_reserve(index, &directory, candidate, transition))
        {
            UnlockPage(index, 0, ExclusiveLock);
            pfree(locations);
            return CUVS_CAGRA_FOLD_DEFERRED;
        }
        xid = U64FromFullTransactionId(transition);
    }

    old_extents = directory.n_extents;
    end = directory.n_slots + candidate->positions;
    offset = directory.n_slots % CUVS_CAGRA_TIDS_PER_PAGE;
    partial = offset ? Min(candidate->positions, CUVS_CAGRA_TIDS_PER_PAGE - offset) : 0;
    nblocks = RelationGetNumberOfBlocks(index);
    if (partial)
        partial_block = fold_slot_block(index, &directory, nblocks, directory.n_slots);
    if (!BlockNumberIsValid(directory.foldlog_tail) || directory.foldlog_tail == 0 ||
        directory.foldlog_tail >= nblocks || directory.foldlog_tail == partial_block)
        bad_page(index, 0);
    log_buffer = ReadBuffer(index, directory.foldlog_tail);
    LockBuffer(log_buffer, BUFFER_LOCK_SHARE);
    tag = fold_log_tail(index, &directory, BufferGetPage(log_buffer));
    rollover = tag->count == CUVS_CAGRA_FOLD_RECORDS_PER_PAGE;
    LockBuffer(log_buffer, BUFFER_LOCK_UNLOCK);

    if (end > (uint64) directory.n_extents * extent_slots)
    {
        BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
        BlockNumber *published = palloc(Max(old_extents, 1) * sizeof(*published));
        BlockNumber next = BlockNumberIsValid(directory.extent_reclaim_from) ?
            directory.extent_reclaim_from : directory.foldlog_head + 1;
        uint32 extent = 0;
        bool search = true;

        if (directory.foldlog_head == 0 || directory.foldlog_head >= nblocks ||
            next <= directory.foldlog_head || next > nblocks)
            bad_page(index, 0);
        memcpy(published, directory.extents, old_extents * sizeof(*published));
        qsort(published, old_extents, sizeof(*published), fold_block_cmp);
        for (uint32 i = 0; i < old_extents; ++i)
            if (published[i] == 0 || published[i] >= nblocks ||
                nblocks - published[i] < CUVS_CAGRA_PAGE_EXTENT ||
                (i && published[i] - published[i - 1] < CUVS_CAGRA_PAGE_EXTENT))
                bad_page(index, 0);
        do
        {
            BlockNumber first = InvalidBlockNumber;

            CHECK_FOR_INTERRUPTS();
            if (search)
            {
                first = fold_find_orphan_extent(index, directory.base_nonce, nblocks,
                    published, old_extents, &next, &extent, strategy);
                search = BlockNumberIsValid(first);
            }
            directory.extents[directory.n_extents] = fold_reserve_extent(index, &directory, first);
            ++directory.n_extents;
        } while (end > (uint64) directory.n_extents * extent_slots);
        /* Advance only in the transition record. On an error or crash the
         * next fold still searches from the old boundary. */
        directory.extent_reclaim_from = next;
        FreeAccessStrategy(strategy);
        pfree(published);
    }
    nblocks = RelationGetNumberOfBlocks(index);
    dead = fold_stage_tid_pages(index, &directory, nblocks, directory.n_slots + partial,
                               end, locations + partial);

#ifdef CUVS_TEST_HOOKS
    if (cuvs_test_abort_staged_fold)
        elog(ERROR, "stopped the fold after staging its TID pages");
#endif

    /* FCL protects the log between its validation and the transition. Do
     * potentially failing extension I/O with no data buffer locks held. */
    if (rollover)
    {
        next_log = extend_page(index, MAIN_FORKNUM);
        LockBuffer(next_log, BUFFER_LOCK_UNLOCK);
    }

    if (!xid)
        xid = U64FromFullTransactionId(GetTopFullTransactionId());
    finished = GetCurrentTimestamp();
    CHECK_FOR_INTERRUPTS();
    state = GenericXLogStart(index);
    if (partial)
    {
        CuvsCagraTidPage *tid_tag;

        tid_buffer = ReadBuffer(index, partial_block);
        LockBuffer(tid_buffer, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(tid_buffer);
        if (PageIsNew(page) || PageGetSpecialSize(page) != sizeof(*tid_tag) ||
            ((PageHeader) page)->pd_lower < MAXALIGN(SizeOfPageHeaderData) +
                offset * sizeof(ItemPointerData) ||
            ((PageHeader) page)->pd_lower > ((PageHeader) page)->pd_upper)
            bad_page(index, partial_block);
        tid_tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
        if (tid_tag->kind != CUVS_PAGE_TIDS || tid_tag->flags != 0 ||
            tid_tag->base_nonce != directory.base_nonce ||
            tid_tag->first_slot != directory.n_slots - offset || tid_tag->dead > offset)
            bad_page(index, partial_block);
        page = GenericXLogRegisterBuffer(state, tid_buffer, 0);
        dead += fold_write_slots(page, offset, partial, locations);
    }

    LockBuffer(log_buffer, BUFFER_LOCK_EXCLUSIVE);
    page = GenericXLogRegisterBuffer(state, log_buffer, 0);
    tag = (CuvsCagraFoldPage *) PageGetSpecialPointer(page);
    if (rollover)
    {
        LockBuffer(next_log, BUFFER_LOCK_EXCLUSIVE);
        tag->next = BufferGetBlockNumber(next_log);
        page = new_page(state, next_log, sizeof(*tag));
        tag = (CuvsCagraFoldPage *) PageGetSpecialPointer(page);
        tag->kind = CUVS_PAGE_FOLDLOG;
        tag->next = InvalidBlockNumber;
    }
    record = (CuvsCagraFoldRecord *) PageGetContents(page) + tag->count++;
    *record = (CuvsCagraFoldRecord) {.graph_nonce = candidate->graph_nonce,
        .n_slots = end, .folded = directory.folded + candidate->positions};
    set_lower(page, tag->count * sizeof(*record));

    /* Lock the metapage last and preserve any concurrent append's tail. */
    meta_buffer = ReadBuffer(index, 0);
    LockBuffer(meta_buffer, BUFFER_LOCK_EXCLUSIVE);
    meta = page_meta(BufferGetPage(meta_buffer));
    if (!meta || !fold_base_matches(meta, candidate) ||
        meta->foldlog_tail != directory.foldlog_tail ||
        meta->n_extents != old_extents ||
        memcmp(meta->extents, directory.extents, old_extents * sizeof(BlockNumber)) != 0 ||
        meta->dead_slots > meta->n_slots)
        bad_page(index, 0);
    meta = page_meta(GenericXLogRegisterBuffer(state, meta_buffer, 0));
    Assert(meta != NULL);
    for (uint32 extent = meta->n_extents; extent < directory.n_extents; ++extent)
        meta->extents[extent] = directory.extents[extent];
    meta->n_extents = directory.n_extents;
    meta->extent_reclaim_from = directory.extent_reclaim_from;
    meta->graph_nonce = candidate->graph_nonce;
    meta->n_slots = end;
    meta->folded += candidate->positions;
    meta->dead_slots += dead;
    meta->last_fold_xid = xid;
    meta->last_fold_time = finished;
    if (BufferIsValid(next_log))
        meta->foldlog_tail = BufferGetBlockNumber(next_log);

    /* This is the irreversible PG publication point. Never undo it on
     * cancellation or restart. Redo decides which complete record is visible. */
    lsn = GenericXLogFinish(state);
    UnlockReleaseBuffer(meta_buffer);
    if (BufferIsValid(next_log))
        UnlockReleaseBuffer(next_log);
    UnlockReleaseBuffer(log_buffer);
    if (BufferIsValid(tid_buffer))
        UnlockReleaseBuffer(tid_buffer);
    UnlockPage(index, 0, ExclusiveLock);
    XLogFlush(lsn);
    *commit_lsn = lsn;
    pfree(locations);
    return CUVS_CAGRA_FOLD_PUBLISHED;
}

CuvsCagraFoldStatus
cuvs_cagra_publish_fold(Relation index, const CuvsCagraFoldCandidate *candidate,
                        XLogRecPtr *commit_lsn)
{
    CuvsCagraFoldStatus status;

    if (!IsBackgroundWorker || !IsTransactionState() || IsTransactionBlock() ||
        IsSubTransaction() || IsInParallelMode() || XactReadOnly || RecoveryInProgress() ||
        TransactionIdIsValid(GetTopTransactionIdIfAny()))
        ereport(ERROR, (errmsg("CAGRA fold publication requires a standalone background worker operation")));
    status = publish_fold(index, candidate, commit_lsn);
    if (status == CUVS_CAGRA_FOLD_DISCARD && RelationNeedsWAL(index) &&
        fold_candidate_valid(candidate))
        return fold_retry_ack(index, candidate, commit_lsn);
    return status;
}

#ifdef CUVS_TEST_HOOKS
static CuvsCagraFoldCandidate
test_fold_candidate(const CuvsCagraMeta *meta, int32 positions, int32 mismatch)
{
    CuvsCagraFoldCandidate candidate = {
        .index_nonce = meta->index_nonce,
        .base_nonce = meta->base_nonce,
        .parent_graph_nonce = meta->graph_nonce,
        .parent_n_slots = meta->n_slots,
        .parent_folded = meta->folded,
        .graph_nonce = cuvs_cagra_nonce(),
        .positions = (uint32) positions,
    };

    if (positions < 0 || (Size) positions > MaxAllocSize / sizeof(CuvsCagraPendingLocation))
        ereport(ERROR, (errmsg("invalid fold test position count")));

    switch (mismatch)
    {
        case 0: break;
        case 1: candidate.index_nonce ^= 1; break;
        case 2: candidate.base_nonce ^= 1; break;
        case 3: candidate.parent_graph_nonce ^= 1; break;
        case 4: ++candidate.parent_n_slots; break;
        case 5: ++candidate.parent_folded; break;
        case 6: candidate.graph_nonce = candidate.parent_graph_nonce; break;
        default: ereport(ERROR, (errmsg("invalid fold test mismatch")));
    }
    return candidate;
}

PG_FUNCTION_INFO_V1(cuvs_test_fold_prepare);

Datum
cuvs_test_fold_prepare(PG_FUNCTION_ARGS)
{
    Relation index = index_open(PG_GETARG_OID(0), RowExclusiveLock);
    int32 positions = PG_GETARG_INT32(1);
    CuvsCagraMeta meta;
    CuvsCagraFoldCandidate candidate;
    CuvsCagraPendingLocation *locations;
    CuvsCagraFoldStatus status;
    StringInfoData result;

    if (RelationGetNumberOfBlocks(index) == 0 || !cuvs_cagra_read_meta(index, &meta))
    {
        index_close(index, RowExclusiveLock);
        PG_RETURN_TEXT_P(cstring_to_text("discard"));
    }
    candidate = test_fold_candidate(&meta, positions, PG_GETARG_INT32(2));
    locations = palloc((Size) positions * sizeof(*locations));
    status = cuvs_cagra_prepare_fold(index, &candidate, &meta, locations);
    if (status != CUVS_CAGRA_FOLD_PREPARED)
    {
        index_close(index, RowExclusiveLock);
        PG_RETURN_TEXT_P(cstring_to_text(status == CUVS_CAGRA_FOLD_DEFERRED ?
                                        "deferred" : "discard"));
    }
    initStringInfo(&result);
    appendStringInfoString(&result, "prepared");
    for (int32 i = 0; i < positions; ++i)
    {
        ItemPointer tid = &locations[i].tid;

        if (ItemPointerIsValid(tid))
            appendStringInfo(&result, " (%u,%u)", ItemPointerGetBlockNumber(tid),
                             ItemPointerGetOffsetNumber(tid));
        else
            appendStringInfoString(&result, " null");
    }
    UnlockPage(index, 0, ExclusiveLock);
    index_close(index, RowExclusiveLock);
    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

PG_FUNCTION_INFO_V1(cuvs_test_fold_publish);

Datum
cuvs_test_fold_publish(PG_FUNCTION_ARGS)
{
    Relation index = index_open(PG_GETARG_OID(0), RowExclusiveLock);
    CuvsCagraMeta meta;
    CuvsCagraFoldCandidate candidate;
    CuvsCagraFoldStatus status;
    XLogRecPtr lsn;

    if (RelationGetNumberOfBlocks(index) == 0 || !cuvs_cagra_read_meta(index, &meta))
    {
        index_close(index, RowExclusiveLock);
        PG_RETURN_TEXT_P(cstring_to_text("discard"));
    }
    candidate = test_fold_candidate(&meta, PG_GETARG_INT32(1), 0);
    /* The fixture can model a synthetic candidate or check the production gate. */
    if (PG_GETARG_BOOL(2))
        status = cuvs_cagra_publish_fold(index, &candidate, &lsn);
    else
        status = publish_fold(index, &candidate, &lsn);
    index_close(index, RowExclusiveLock);
    if (status == CUVS_CAGRA_FOLD_PUBLISHED)
    {
        if (XLogRecPtrIsInvalid(lsn) || lsn > GetFlushRecPtr(NULL))
            ereport(ERROR, (errmsg("fold returned an unflushed publication LSN")));
        PG_RETURN_TEXT_P(cstring_to_text(psprintf("published %X/%X", LSN_FORMAT_ARGS(lsn))));
    }
    PG_RETURN_TEXT_P(cstring_to_text(status == CUVS_CAGRA_FOLD_DEFERRED ?
                                    "deferred" : "discard"));
}

PG_FUNCTION_INFO_V1(cuvs_test_pending_first);

Datum
cuvs_test_pending_first(PG_FUNCTION_ARGS)
{
    Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
    int64 requested = PG_GETARG_INT64(1);
    CuvsCagraMeta meta;
    CuvsCagraPendingLocation location;
    float *vector;
    bool valid;

    if (requested < 0 || !cuvs_cagra_read_meta(index, &meta) ||
        !(meta.flags & CUVS_CAGRA_PENDING_VECTOR))
        ereport(ERROR, (errmsg("expected a vector-bearing CAGRA pending list")));
    if (PG_NARGS() == 4)
    {
        int32 head = PG_GETARG_INT32(2);
        int64 folded = PG_GETARG_INT64(3);

        if (head <= 0 || folded < 0)
            ereport(ERROR, (errmsg("invalid test pending head")));
        /* Simulate the metadata snapshot after a fold moves the head page. */
        meta.pending_head = (BlockNumber) head;
        meta.folded = (uint64) folded;
    }
    vector = palloc((Size) meta.dim * sizeof(float));
    valid = cuvs_cagra_copy_pending(index, &meta, RelationGetNumberOfBlocks(index),
                                   (uint64) requested, (uint64) requested + 1,
                                   &location, vector);
    index_close(index, AccessShareLock);
    if (!valid || !ItemPointerIsValid(&location.tid))
        ereport(ERROR, (errmsg("could not read the CAGRA pending vector")));
    PG_RETURN_FLOAT8((double) vector[0]);
}
#endif

CuvsCagraSlotResult
cuvs_cagra_pending_tid(Relation index, const CuvsCagraMeta *meta, BlockNumber nblocks,
                       BlockNumber block, uint64 position, ItemPointer tid)
{
    Buffer buf;
    Page page;
    const CuvsCagraPending *tag;
    CuvsCagraSlotResult result = CUVS_CAGRA_SLOT_BAD;
    uint32 vector_dim = pending_vector_dim(meta);

    if (block == 0 || block >= nblocks || position < meta->folded)
        return result;
    buf = ReadBuffer(index, block);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    tag = pending_tag(page, block, meta->index_nonce, vector_dim);
    if (tag && position >= tag->first_pos && position - tag->first_pos < tag->count)
    {
        const char *entry = (char *) PageGetContents(page) +
            (position - tag->first_pos) * pending_stride(vector_dim);

        memcpy(tid, entry, sizeof(*tid));
        result = ItemPointerIsValid(tid) ? CUVS_CAGRA_SLOT_LIVE : CUVS_CAGRA_SLOT_DEAD;
    }
    UnlockReleaseBuffer(buf);
    return result;
}

static bool
read_slots(Relation index, const CuvsCagraMeta *meta,
                    BlockNumber nblocks, uint64 slot, uint32 count, ItemPointer tids)
{
    uint64 page_no = slot / CUVS_CAGRA_TIDS_PER_PAGE;
    uint64 extent = page_no / CUVS_CAGRA_PAGE_EXTENT;
    uint32 offset = slot % CUVS_CAGRA_TIDS_PER_PAGE;
    BlockNumber start, block;
    Buffer buf;
    Page page;
    const CuvsCagraTidPage *tag;
    bool valid = false;

    if (slot >= meta->n_slots || count > meta->n_slots - slot ||
        extent >= meta->n_extents || count > CUVS_CAGRA_TIDS_PER_PAGE - offset)
        return false;
    start = meta->extents[extent];
    if (start == 0 || start >= InvalidBlockNumber - CUVS_CAGRA_PAGE_EXTENT)
        return false;
    block = start + page_no % CUVS_CAGRA_PAGE_EXTENT;
    if (block >= nblocks)
        return false;

    buf = ReadBuffer(index, block);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    if (!PageIsNew(page) && PageGetSpecialSize(page) == sizeof(*tag) &&
        ((PageHeader) page)->pd_lower >= MAXALIGN(SizeOfPageHeaderData) +
            (offset + count) * sizeof(ItemPointerData) &&
        ((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper)
    {
        tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
        if (tag->kind == CUVS_PAGE_TIDS && tag->base_nonce == meta->base_nonce &&
            tag->first_slot == slot - offset && (tag->flags & ~1U) == 0)
        {
            memcpy(tids, (ItemPointerData *) PageGetContents(page) + offset,
                   count * sizeof(ItemPointerData));
            valid = true;
        }
    }
    UnlockReleaseBuffer(buf);
    return valid;
}


CuvsCagraSlotResult
cuvs_cagra_slot_tid(Relation index, const CuvsCagraMeta *meta,
                    BlockNumber nblocks, uint64 slot, ItemPointer tid)
{
    if (!read_slots(index, meta, nblocks, slot, 1, tid))
        return CUVS_CAGRA_SLOT_BAD;
    return ItemPointerIsValid(tid) ? CUVS_CAGRA_SLOT_LIVE : CUVS_CAGRA_SLOT_DEAD;
}

static int
compare_root(const void *a, const void *b)
{
    uint64 x = *(const uint64 *) a;
    uint64 y = *(const uint64 *) b;

    return (x > y) - (x < y);
}

bool
cuvs_cagra_slot_filter(Relation index, const CuvsCagraMeta *meta,
                      BlockNumber nblocks, const uint64 *roots, int nroots,
                      uint32 *excluded)
{
    ItemPointerData segment[CUVS_CAGRA_TIDS_PER_PAGE];

    memset(excluded, 0xff, ((meta->n_slots + 31) / 32) * sizeof(*excluded));
    for (uint64 slot = 0; slot < meta->n_slots; slot += CUVS_CAGRA_TIDS_PER_PAGE)
    {
        uint32 count = Min((uint64) CUVS_CAGRA_TIDS_PER_PAGE, meta->n_slots - slot);

        CHECK_FOR_INTERRUPTS();
        if (!read_slots(index, meta, nblocks, slot, count, segment))
            return false;
        for (uint32 i = 0; i < count; ++i)
        {
            ItemPointer tid = &segment[i];
            uint64 root;

            if (!ItemPointerIsValid(tid))
                continue;
            root = cuvs_tid_encode(ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));
            if (bsearch(&root, roots, nroots, sizeof(root), compare_root))
                excluded[(slot + i) / 32] &= ~((uint32) 1 << ((slot + i) % 32));
        }
    }
    return true;
}

/* The ANN window is exhausted. Enumerate only the selected graph's index
 * slots, and inspect visibility without reading ranking vectors. */
bool
cuvs_cagra_snapshot_filter(Relation index, Relation heap, Snapshot snapshot,
                            const CuvsCagraMeta *meta, BlockNumber blocks, uint64 limit,
                            uint32 *excluded, uint64 *visible,
                            bool (*visit)(void *), void *visit_arg)
{
    ItemPointerData segment[CUVS_CAGRA_TIDS_PER_PAGE];
    IndexFetchTableData *fetch = table_index_fetch_begin(heap);
    TupleTableSlot *slot = table_slot_create(heap, NULL);
    bool valid = true;

    *visible = 0;
    memset(excluded, 0xff, ((limit + 31) / 32) * sizeof(*excluded));
    for (uint64 pos = 0; pos < limit; pos += CUVS_CAGRA_TIDS_PER_PAGE)
    {
        uint32 count = Min((uint64) CUVS_CAGRA_TIDS_PER_PAGE, limit - pos);

        if (!visit(visit_arg) || !read_slots(index, meta, blocks, pos, count, segment))
        {
            valid = false;
            break;
        }
        for (uint32 i = 0; i < count; ++i)
        {
            bool again = false, dead = false;

            CHECK_FOR_INTERRUPTS();
            if (!ItemPointerIsValid(&segment[i]))
                continue;
            table_index_fetch_reset(fetch);
            if (table_index_fetch_tuple(fetch, &segment[i], snapshot, slot, &again, &dead))
            {
                excluded[(pos + i) / 32] &= ~((uint32) 1 << ((pos + i) % 32));
                ++*visible;
            }
        }
    }
    table_index_fetch_end(fetch);
    ExecDropSingleTupleTableSlot(slot);
    return valid;
}

static void
append_item(Page page, ItemPointer tid, const float *vector)
{
    CuvsCagraPending *pending = (CuvsCagraPending *) PageGetSpecialPointer(page);
    Size stride = pending_stride(pending->vector_dim);
    char *item = (char *) PageGetContents(page) + stride * pending->count;

    memcpy(item, tid, sizeof(*tid));
    if (pending->vector_dim)
        memcpy(item + sizeof(*tid), vector, (Size) pending->vector_dim * sizeof(float));
    ++pending->count;
    set_lower(page, pending->count * stride);
}

void
cuvs_cagra_append_pending(Relation index, ItemPointer tid, int dim, const float *vector)
{
    /* Buffer operations may outlive an rd_amcache allocation. */
    CuvsCagraRelCache cached = *cuvs_cagra_relation_cache(index);
    BlockNumber tail;
    uint64 next_pos = 0;
    uint32 vector_dim, capacity;

    if (cached.index_nonce == 0)
    {
        CuvsCagraMeta meta;

        if (!cuvs_cagra_read_meta(index, &meta))
            bad_page(index, 0);
        cached.tail = meta.pending_tail;
        cached.index_nonce = meta.index_nonce;
        cached.dim = meta.dim;
        cached.pending_vectors = (meta.flags & CUVS_CAGRA_PENDING_VECTOR) != 0;
    }
    cuvs_cagra_check_dim(cached.dim, dim);
    vector_dim = cached.pending_vectors ? dim : 0;
    capacity = pending_capacity(vector_dim);
    tail = cached.tail;

    for (;;)
    {
        Buffer tb = ReadBuffer(index, tail);
        Buffer mb = InvalidBuffer;
        Buffer nb = InvalidBuffer;
        CuvsCagraPending *pending;
        CuvsCagraMeta *meta = NULL;
        GenericXLogState *wal;
        Page image;
        bool full;
        CuvsCagraRelCache *cache;

        LockBuffer(tb, BUFFER_LOCK_EXCLUSIVE);
        pending = page_pending(index, tail, BufferGetPage(tb), cached.index_nonce, vector_dim);
        if (next_pos != 0 && pending->first_pos != next_pos)
            bad_page(index, tail);
        if (BlockNumberIsValid(pending->next))
        {
            if (pending->count != capacity ||
                pending->first_pos > UINT64_MAX - pending->count)
                bad_page(index, tail);
            next_pos = pending->first_pos + pending->count;
            tail = pending->next;
            UnlockReleaseBuffer(tb);
            continue;
        }

        full = pending->count == capacity;
        if (full && cached.dim == 0)
        {
            CuvsCagraMeta current;

            if (!cuvs_cagra_read_meta(index, &current) ||
                current.index_nonce != cached.index_nonce || current.pending_tail != tail)
                bad_page(index, 0);
            cuvs_cagra_check_dim(current.dim, dim);
        }
        if (full)
            nb = extend_page(index, MAIN_FORKNUM);
        if (cached.dim == 0 || full)
        {
            mb = ReadBuffer(index, 0);
            LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
            meta = page_meta(BufferGetPage(mb));
            if (meta == NULL || meta->index_nonce != cached.index_nonce ||
                meta->pending_tail != tail)
                bad_page(index, 0);
            cuvs_cagra_check_dim(meta->dim, dim);
        }

        wal = GenericXLogStart(index);
        image = GenericXLogRegisterBuffer(wal, tb, 0);
        if (full)
        {
            CuvsCagraPending *old = (CuvsCagraPending *) PageGetSpecialPointer(image);
            BlockNumber next = BufferGetBlockNumber(nb);

            old->next = next;
            image = new_page(wal, nb, sizeof(*pending));
            init_pending(image, cached.index_nonce, old->first_pos + old->count, vector_dim);
            tail = next;
        }
        append_item(image, tid, vector);
        if (meta != NULL)
        {
            CuvsCagraMeta *updated = (CuvsCagraMeta *)
                PageGetContents(GenericXLogRegisterBuffer(wal, mb, 0));

            updated->dim = dim;
            updated->pending_tail = tail;
        }
        GenericXLogFinish(wal);
        if (BufferIsValid(mb))
            UnlockReleaseBuffer(mb);
        if (BufferIsValid(nb))
            UnlockReleaseBuffer(nb);
        UnlockReleaseBuffer(tb);
        cache = cuvs_cagra_relation_cache(index);
        cache->index_nonce = cached.index_nonce;
        cache->tail = tail;
        cache->dim = dim;
        cache->pending_vectors = cached.pending_vectors;
        return;
    }
}

typedef struct CagraVacuum
{
    Relation                index;
    IndexBulkDeleteCallback callback;
    void                   *callback_state;
    GenericXLogState       *wal;
    Buffer                  held[MAX_GENERIC_XLOG_PAGES];
    int                     nheld;
    uint64                  removed;
    uint64                  live;
    uint64                  dead_slots;
} CagraVacuum;

#ifdef CUVS_TEST_HOOKS
static bool cuvs_test_pause_page_vacuum;

void
cuvs_cagra_init_test_hooks(void)
{
    DefineCustomBoolVariable("cuvs.test_pause_page_vacuum",
                             "Pause a page-format VACUUM after the TID records.",
                             NULL, &cuvs_test_pause_page_vacuum, false, PGC_SUSET,
                             GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
    DefineCustomBoolVariable("cuvs.test_abort_staged_fold",
                             "Abort a page-format fold after staging its TID pages.",
                             NULL, &cuvs_test_abort_staged_fold, false, PGC_SUSET,
                             GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
#endif

/* Record only dirty pages; retain their locks until this batch is published. */
static void
vacuum_finish_batch(CagraVacuum *vac)
{
    if (vac->wal)
    {
        GenericXLogFinish(vac->wal);
        vac->wal = NULL;
    }
    while (vac->nheld)
        UnlockReleaseBuffer(vac->held[--vac->nheld]);
}

static Buffer
vacuum_lock(CagraVacuum *vac, BlockNumber block)
{
    Buffer buf;

    if (vac->nheld == lengthof(vac->held))
        vacuum_finish_batch(vac);
    CHECK_FOR_INTERRUPTS();
    buf = ReadBuffer(vac->index, block);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    vac->held[vac->nheld++] = buf;
    return buf;
}

static void
vacuum_tids(CagraVacuum *vac, Buffer buf, uint32 start, uint32 count,
            Size stride, const CuvsCagraTidPage *tid_tag)
{
    Page page = BufferGetPage(buf);
    const char *items = (const char *) PageGetContents(page);
    Page image = NULL;
    uint32 old_dead = 0;
    uint32 removed = 0;

    for (uint32 i = start; i < count; ++i)
    {
        ItemPointerData tid;

        memcpy(&tid, items + i * stride, sizeof(tid));
        if (!ItemPointerIsValid(&tid))
        {
            ++old_dead;
            continue;
        }
        if (!vac->callback(&tid, vac->callback_state))
        {
            ++vac->live;
            continue;
        }
        if (!image)
        {
            if (!vac->wal)
                vac->wal = GenericXLogStart(vac->index);
            image = GenericXLogRegisterBuffer(vac->wal, buf, 0);
        }
        ItemPointerSetInvalid(&tid);
        memcpy((char *) PageGetContents(image) + i * stride, &tid, sizeof(tid));
        ++removed;
    }

    vac->removed += removed;
    if (tid_tag)
    {
        if (old_dead != tid_tag->dead)
            bad_page(vac->index, BufferGetBlockNumber(buf));
        if (image)
            ((CuvsCagraTidPage *) PageGetSpecialPointer(image))->dead = old_dead + removed;
        vac->dead_slots += old_dead + removed;
    }
}

static void
vacuum_pending(CagraVacuum *vac, const CuvsCagraMeta *meta)
{
    BlockNumber block = meta->pending_head;
    uint64 expected = 0;
    uint32 vector_dim = pending_vector_dim(meta);

    for (;;)
    {
        Buffer buf = vacuum_lock(vac, block);
        CuvsCagraPending *tag = page_pending(vac->index, block, BufferGetPage(buf),
                                            meta->index_nonce, vector_dim);
        uint32 start = 0;

        if ((expected && tag->first_pos != expected) ||
            tag->first_pos > UINT64_MAX - tag->count)
            bad_page(vac->index, block);
        if (meta->folded > tag->first_pos)
            start = Min(meta->folded - tag->first_pos, (uint64) tag->count);
        vacuum_tids(vac, buf, start, tag->count, pending_stride(vector_dim), NULL);
        /* Inserts can append after the metapage snapshot. Their TIDs are not
         * old enough to be removed by this VACUUM. Validation also only needs
         * pending entries present when it read the metapage. */
        if (block == meta->pending_tail)
            break;
        if (!BlockNumberIsValid(tag->next) || tag->count != pending_capacity(vector_dim))
            bad_page(vac->index, block);
        expected = tag->first_pos + tag->count;
        block = tag->next;
    }
}

static void
vacuum_slots(CagraVacuum *vac, const CuvsCagraMeta *meta)
{
    uint64 slot = 0;

    for (uint32 extent = 0; slot < meta->n_slots; ++extent)
    {
        BlockNumber base;

        if (extent >= meta->n_extents)
            bad_page(vac->index, 0);
        base = meta->extents[extent];
        if (base == 0 ||
            base >= InvalidBlockNumber - CUVS_CAGRA_PAGE_EXTENT)
            bad_page(vac->index, 0);
        for (uint32 i = 0; i < CUVS_CAGRA_PAGE_EXTENT && slot < meta->n_slots; ++i)
        {
            BlockNumber block = base + i;
            uint32 count = Min((uint64) CUVS_CAGRA_TIDS_PER_PAGE, meta->n_slots - slot);
            Buffer buf = vacuum_lock(vac, block);
            Page page = BufferGetPage(buf);
            PageHeader header = (PageHeader) page;
            const CuvsCagraTidPage *tag;

            if (PageIsNew(page) || PageGetSpecialSize(page) != sizeof(*tag) ||
                header->pd_lower < MAXALIGN(SizeOfPageHeaderData) + count * sizeof(ItemPointerData) ||
                header->pd_lower > header->pd_upper)
                bad_page(vac->index, block);
            tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
            if (tag->kind != CUVS_PAGE_TIDS || tag->base_nonce != meta->base_nonce ||
                tag->first_slot != slot || tag->flags != 0 || tag->dead > count)
                bad_page(vac->index, block);
            vacuum_tids(vac, buf, 0, count, sizeof(ItemPointerData), tag);
            slot += count;
        }
    }
}

IndexBulkDeleteResult *
cuvs_cagra_page_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                           IndexBulkDeleteCallback callback, void *callback_state)
{
    Relation index = info->index;
    CuvsCagraMeta meta;
    CagraVacuum vac = {.index = index, .callback = callback, .callback_state = callback_state};
    Buffer mb;
    CuvsCagraMeta *updated;
    GenericXLogState *wal;

    if (stats == NULL)
        stats = palloc0(sizeof(*stats));

    LockPage(index, 0, ExclusiveLock);
    if (!cuvs_cagra_read_meta(index, &meta))
        bad_page(index, 0);
    vacuum_pending(&vac, &meta);
    vacuum_slots(&vac, &meta);
    vacuum_finish_batch(&vac);

#ifdef CUVS_TEST_HOOKS
    if (cuvs_test_pause_page_vacuum && vac.removed)
    {
        elog(LOG, "page-format VACUUM paused before publishing its generation");
        for (int i = 0; i < 3000; ++i)
        {
            CHECK_FOR_INTERRUPTS();
            pg_usleep(10000L);
        }
    }
#endif

    mb = ReadBuffer(index, 0);
    LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
    updated = page_meta(BufferGetPage(mb));
    if (updated == NULL || updated->index_nonce != meta.index_nonce ||
        updated->base_nonce != meta.base_nonce || updated->delete_generation == UINT64_MAX)
        bad_page(index, 0);
    wal = GenericXLogStart(index);
    updated = (CuvsCagraMeta *) PageGetContents(GenericXLogRegisterBuffer(wal, mb, 0));
    /* A canceled predecessor can already have invalidated the slots. The
     * heap can free those TIDs after this callback returns even if this call
     * changed no data page, so always advance the generation. */
    ++updated->delete_generation;
    updated->dead_slots = vac.dead_slots;
    GenericXLogFinish(wal);
    UnlockReleaseBuffer(mb);
    UnlockPage(index, 0, ExclusiveLock);

    stats->num_pages = RelationGetNumberOfBlocks(index);
    stats->estimated_count = true;
    stats->num_index_tuples = vac.live;
    stats->tuples_removed += vac.removed;
    return stats;
}

IndexBulkDeleteResult *
cuvs_cagra_page_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    CuvsCagraMeta meta;

    if (info->analyze_only)
        return stats;

    LockPage(info->index, 0, ExclusiveLock);
    if (!cuvs_cagra_read_meta(info->index, &meta))
        bad_page(info->index, 0);
    if (stats != NULL)
        stats->num_pages = RelationGetNumberOfBlocks(info->index);
    UnlockPage(info->index, 0, ExclusiveLock);
    return stats;
}

#endif

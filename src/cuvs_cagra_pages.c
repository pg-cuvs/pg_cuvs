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
#include "access/htup_details.h"
#include "access/xloginsert.h"
#include "cuvs_util.h"
#include "storage/bufpage.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"

#ifdef CUVS_TEST_HOOKS
#include "fmgr.h"
#endif

enum CuvsCagraPageKind
{
    CUVS_PAGE_TIDS = 1,
    CUVS_PAGE_PENDING = 2,
    CUVS_PAGE_FOLDLOG = 3
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
    uint32      reserved;
    uint64      index_nonce;
    uint64      first_pos;
} CuvsCagraPending;

typedef struct CuvsCagraFoldPage
{
    uint16      kind;
    uint16      flags;
    uint32      count;
    BlockNumber next;
} CuvsCagraFoldPage;

typedef struct CuvsCagraFoldRecord
{
    uint64      graph_nonce;
    uint64      n_slots;
    uint64      folded;
} CuvsCagraFoldRecord;

#define CAGRA_META_PREFIX_SIZE 12

StaticAssertDecl(offsetof(CuvsCagraMeta, magic) == 0 &&
                 offsetof(CuvsCagraMeta, major) == 4 &&
                 offsetof(CuvsCagraMeta, minor) == 6 &&
                 offsetof(CuvsCagraMeta, flags) + sizeof(uint32) == CAGRA_META_PREFIX_SIZE,
                 "CAGRA common metapage prefix must not change");
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

static bool
page_bounds_valid(Page page)
{
    PageHeader header = (PageHeader) page;

    return !PageIsNew(page) && PageGetPageSize(page) == BLCKSZ &&
        PageGetPageLayoutVersion(page) == PG_PAGE_LAYOUT_VERSION &&
        header->pd_lower >= MAXALIGN(SizeOfPageHeaderData) &&
        header->pd_lower <= header->pd_upper && header->pd_upper <= header->pd_special &&
        header->pd_special <= BLCKSZ;
}

/* The common prefix is independent of the format version and payload length. */
static CuvsCagraMeta *
page_meta_prefix(Page page)
{
    PageHeader header = (PageHeader) page;
    CuvsCagraMeta *prefix;

    if (!page_bounds_valid(page) ||
        header->pd_lower < MAXALIGN(SizeOfPageHeaderData) + CAGRA_META_PREFIX_SIZE)
        return NULL;
    prefix = (CuvsCagraMeta *) PageGetContents(page);
    return prefix->magic == CUVS_CAGRA_PAGE_MAGIC ? prefix : NULL;
}

static CuvsCagraMeta *
page_meta(Page page)
{
    CuvsCagraMeta *meta = page_meta_prefix(page);
    PageHeader header = (PageHeader) page;

    if (meta == NULL || header->pd_lower < MAXALIGN(SizeOfPageHeaderData) + sizeof(*meta))
        return NULL;
    /* This format accepts no flags. REINDEX_REQUIRED also makes known layouts unusable. */
    if (meta->major != CUVS_CAGRA_PAGE_MAJOR || meta->minor != 0 || meta->flags != 0 ||
        meta->index_nonce == 0 || meta->base_nonce == 0 ||
        meta->n_extents > CUVS_CAGRA_PAGE_DIR ||
        meta->folded > meta->n_slots || meta->base_slots != meta->n_slots - meta->folded ||
        meta->n_slots > (uint64) meta->n_extents * CUVS_CAGRA_PAGE_EXTENT * CUVS_CAGRA_TIDS_PER_PAGE ||
        !BlockNumberIsValid(meta->pending_head) || meta->pending_head == 0 ||
        !BlockNumberIsValid(meta->pending_tail) || meta->pending_tail == 0)
        return NULL;
    return meta;
}

/* PG16 leaves a failed read's pin and buffer I/O in the resource owner.
 * Catch only page corruption and release those resources before continuing. */
static void
discard_corrupt_read(ResourceOwner reader, MemoryContext context)
{
    if (geterrcode() != ERRCODE_DATA_CORRUPTED)
        PG_RE_THROW();
    MemoryContextSwitchTo(context);
    FlushErrorState();
    ResourceOwnerRelease(reader, RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
}

static Buffer
read_meta_buffer(Relation index, BufferAccessStrategy strategy)
{
    MemoryContext context = CurrentMemoryContext;
    ResourceOwner caller = CurrentResourceOwner;
    ResourceOwner reader = ResourceOwnerCreate(caller, "CAGRA metapage read");
    Buffer buf = InvalidBuffer;

    PG_TRY();
    {
        CurrentResourceOwner = reader;
        buf = ReadBufferExtended(index, MAIN_FORKNUM, 0, RBM_NORMAL, strategy);
    }
    PG_CATCH();
    {
        CurrentResourceOwner = caller;
        discard_corrupt_read(reader, context);
        buf = InvalidBuffer;
    }
    PG_END_TRY();
    CurrentResourceOwner = caller;

    if (BufferIsValid(buf))
    {
        IncrBufferRefCount(buf);
        CurrentResourceOwner = reader;
        ReleaseBuffer(buf);
        CurrentResourceOwner = caller;
    }
    ResourceOwnerDelete(reader);
    return buf;
}

static bool
read_cagra_meta(Relation index, BufferAccessStrategy strategy, CuvsCagraMeta *meta,
                bool *prefix_readable)
{
    Buffer buf = read_meta_buffer(index, strategy);
    CuvsCagraMeta *found;

    if (prefix_readable)
        *prefix_readable = false;
    if (!BufferIsValid(buf))
        return false;
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    found = page_meta(BufferGetPage(buf));
    if (found != NULL)
        memcpy(meta, found, sizeof(*meta));
    if (prefix_readable)
        *prefix_readable = found != NULL || page_meta_prefix(BufferGetPage(buf)) != NULL;
    UnlockReleaseBuffer(buf);
    return found != NULL;
}

bool
cuvs_cagra_read_meta(Relation index, CuvsCagraMeta *meta)
{
    return read_cagra_meta(index, NULL, meta, NULL);
}

bool
cuvs_cagra_reindex_required(Relation index)
{
    Buffer buf;
    CuvsCagraMeta *prefix;
    bool required;

    if (!cuvs_cagra_has_pages(index))
        return false;
    buf = read_meta_buffer(index, NULL);
    if (!BufferIsValid(buf))
        return false;
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    prefix = page_meta_prefix(BufferGetPage(buf));
    required = prefix != NULL && (prefix->flags & CUVS_CAGRA_REINDEX_REQUIRED) != 0;
    UnlockReleaseBuffer(buf);
    return required;
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
init_pending(Page page, uint64 nonce, uint64 first_pos)
{
    CuvsCagraPending *pending = (CuvsCagraPending *) PageGetSpecialPointer(page);

    pending->kind = CUVS_PAGE_PENDING;
    pending->index_nonce = nonce;
    pending->first_pos = first_pos;
    pending->next = InvalidBlockNumber;
    return pending;
}

void
cuvs_cagra_init_pages(Relation index, ForkNumber fork, uint64 index_nonce,
                      uint64 graph_nonce, int dim, uint32 metric,
                      const uint64 *tids, int64 n)
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
    meta.dim = dim;
    meta.metric = metric;
    meta.index_nonce = index_nonce;
    meta.graph_nonce = graph_nonce;
    meta.base_nonce = graph_nonce != 0 ? graph_nonce : cuvs_cagra_nonce();
    meta.n_slots = n;
    meta.base_slots = n;
    meta.extent_overflow = InvalidBlockNumber;
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
    init_pending(page, index_nonce, 0);
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
pending_tag(Page page, BlockNumber block, uint64 nonce)
{
    CuvsCagraPending *pending;
    PageHeader header = (PageHeader) page;

    if (!page_bounds_valid(page) || PageGetSpecialSize(page) != sizeof(*pending))
        return NULL;
    pending = (CuvsCagraPending *) PageGetSpecialPointer(page);
    if (pending->kind != CUVS_PAGE_PENDING || pending->index_nonce != nonce ||
        pending->flags != 0 || pending->count > CUVS_CAGRA_PENDING_PER_PAGE ||
        header->pd_lower != MAXALIGN(SizeOfPageHeaderData) + pending->count * sizeof(ItemPointerData) ||
        pending->next == block || pending->next == 0)
        return NULL;
    return pending;
}

static CuvsCagraPending *
page_pending(Relation index, BlockNumber block, Page page, uint64 nonce)
{
    CuvsCagraPending *pending = pending_tag(page, block, nonce);

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
    tag = pending_tag(BufferGetPage(buf), meta->pending_tail, meta->index_nonce);
    valid = tag && !BlockNumberIsValid(tag->next) &&
        tag->first_pos <= UINT64_MAX - tag->count;
    if (valid)
        *tail = tag->first_pos + tag->count;
    UnlockReleaseBuffer(buf);
    return valid;
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
    if (page_bounds_valid(page) && PageGetSpecialSize(page) == sizeof(*tag) &&
        ((PageHeader) page)->pd_lower >= MAXALIGN(SizeOfPageHeaderData) +
            (offset + count) * sizeof(ItemPointerData))
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

static void
append_item(Page page, ItemPointer tid)
{
    CuvsCagraPending *pending = (CuvsCagraPending *) PageGetSpecialPointer(page);
    ItemPointerData *items = (ItemPointerData *) PageGetContents(page);

    items[pending->count++] = *tid;
    set_lower(page, pending->count * sizeof(*items));
}

void
cuvs_cagra_append_pending(Relation index, ItemPointer tid, int dim)
{
    /* Buffer operations may outlive an rd_amcache allocation. */
    CuvsCagraRelCache cached = *cuvs_cagra_relation_cache(index);
    BlockNumber tail;
    uint64 next_pos = 0;

    if (cached.index_nonce == 0)
    {
        CuvsCagraMeta meta;

        if (!cuvs_cagra_read_meta(index, &meta))
            bad_page(index, 0);
        cached.tail = meta.pending_tail;
        cached.index_nonce = meta.index_nonce;
        cached.dim = meta.dim;
    }
    cuvs_cagra_check_dim(cached.dim, dim);
    tail = cached.tail;

    for (;;)
    {
        Buffer tb = ReadBuffer(index, tail);
        Buffer mb;
        Buffer nb = InvalidBuffer;
        CuvsCagraPending *pending;
        CuvsCagraMeta *meta;
        GenericXLogState *wal;
        Page image;
        bool full;
        bool update_meta;
        CuvsCagraRelCache *cache;

        LockBuffer(tb, BUFFER_LOCK_EXCLUSIVE);
        pending = page_pending(index, tail, BufferGetPage(tb), cached.index_nonce);
        if (next_pos != 0 && pending->first_pos != next_pos)
            bad_page(index, tail);
        if (BlockNumberIsValid(pending->next))
        {
            if (pending->count != CUVS_CAGRA_PENDING_PER_PAGE ||
                pending->first_pos > UINT64_MAX - pending->count)
                bad_page(index, tail);
            next_pos = pending->first_pos + pending->count;
            tail = pending->next;
            UnlockReleaseBuffer(tb);
            continue;
        }

        full = pending->count == CUVS_CAGRA_PENDING_PER_PAGE;
        update_meta = cached.dim == 0 || full;
        /* Hold the metapage lock through publication. A cached tail must
         * still reject writes after another backend marks the index. */
        mb = ReadBuffer(index, 0);
        LockBuffer(mb, update_meta ? BUFFER_LOCK_EXCLUSIVE : BUFFER_LOCK_SHARE);
        meta = page_meta(BufferGetPage(mb));
        if (meta == NULL || meta->index_nonce != cached.index_nonce ||
            meta->pending_tail != tail)
            bad_page(index, 0);
        cuvs_cagra_check_dim(meta->dim, dim);
        if (full)
            nb = extend_page(index, MAIN_FORKNUM);

        wal = GenericXLogStart(index);
        image = GenericXLogRegisterBuffer(wal, tb, 0);
        if (full)
        {
            CuvsCagraPending *old = (CuvsCagraPending *) PageGetSpecialPointer(image);
            BlockNumber next = BufferGetBlockNumber(nb);

            old->next = next;
            image = new_page(wal, nb, sizeof(*pending));
            init_pending(image, cached.index_nonce, old->first_pos + old->count);
            tail = next;
        }
        append_item(image, tid);
        if (update_meta)
        {
            CuvsCagraMeta *updated = (CuvsCagraMeta *)
                PageGetContents(GenericXLogRegisterBuffer(wal, mb, 0));

            updated->dim = dim;
            updated->pending_tail = tail;
        }
        GenericXLogFinish(wal);
        UnlockReleaseBuffer(mb);
        if (BufferIsValid(nb))
            UnlockReleaseBuffer(nb);
        UnlockReleaseBuffer(tb);
        cache = cuvs_cagra_relation_cache(index);
        cache->index_nonce = cached.index_nonce;
        cache->tail = tail;
        cache->dim = dim;
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
    BlockNumber             nblocks;
    BlockNumber             block;
    BufferAccessStrategy    strategy;
    bool                    validate_only;
    uint64                  removed;
    uint64                  live;
    uint64                  dead_slots;
} CagraVacuum;

#ifdef CUVS_TEST_HOOKS
PG_FUNCTION_INFO_V1(cuvs_test_cagra_reindex_required);

Datum
cuvs_test_cagra_reindex_required(PG_FUNCTION_ARGS)
{
    Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
    bool required = cuvs_cagra_reindex_required(index);

    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(required);
}

static bool cuvs_test_pause_page_vacuum;

void
cuvs_cagra_vacuum_init_test_hook(void)
{
    DefineCustomBoolVariable("cuvs.test_pause_page_vacuum",
                             "Pause a page-format VACUUM after the TID records.",
                             NULL, &cuvs_test_pause_page_vacuum, false, PGC_SUSET,
                             GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
#endif

static List *vacuum_warned_indexes = NIL;

static void
vacuum_reset_warnings(void *arg)
{
    vacuum_warned_indexes = NIL;
}

/* Both VACUUM callbacks can visit each index, including multiple bulk-delete
 * rounds. Keep all warned OIDs for the command, even when they interleave. */
static void
vacuum_warn_reindex(Relation index)
{
    Oid oid = RelationGetRelid(index);
    MemoryContext old;

    if (list_member_oid(vacuum_warned_indexes, oid))
        return;

    old = MemoryContextSwitchTo(TopTransactionContext);
    if (vacuum_warned_indexes == NIL)
    {
        MemoryContextCallback *reset = palloc0(sizeof(*reset));

        reset->func = vacuum_reset_warnings;
        MemoryContextRegisterResetCallback(TopTransactionContext, reset);
    }
    vacuum_warned_indexes = lappend_oid(vacuum_warned_indexes, oid);
    MemoryContextSwitchTo(old);
    ereport(WARNING,
            (errmsg("VACUUM skipped unrecognized or damaged CAGRA index \"%s\"",
                    RelationGetRelationName(index)),
             errhint("REINDEX INDEX %s using a build that supports the page format.",
                     quote_qualified_identifier(get_namespace_name(RelationGetNamespace(index)),
                                                RelationGetRelationName(index)))));
}

static void
vacuum_mark_reindex(Relation index, BufferAccessStrategy strategy, bool prefix_readable)
{
    Buffer buf = InvalidBuffer;
    CuvsCagraMeta *prefix = NULL;

    /* Count the earlier read if it failed to read the common prefix. */
    for (int attempts = prefix_readable ? 2 : 1; attempts > 0; --attempts)
    {
        buf = read_meta_buffer(index, strategy);
        if (!BufferIsValid(buf))
            continue;
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        prefix = page_meta_prefix(BufferGetPage(buf));
        if (prefix != NULL || attempts == 1)
            break;
        UnlockReleaseBuffer(buf);
        buf = InvalidBuffer;
    }
    if (!BufferIsValid(buf))
    {
        buf = ReadBufferExtended(index, MAIN_FORKNUM, 0, RBM_ZERO_AND_LOCK, strategy);
        prefix = page_meta_prefix(BufferGetPage(buf));
    }

    if (prefix == NULL || !(prefix->flags & CUVS_CAGRA_REINDEX_REQUIRED))
    {
        GenericXLogState *wal = GenericXLogStart(index);
        Page image = GenericXLogRegisterBuffer(wal, buf, prefix == NULL ? GENERIC_XLOG_FULL_IMAGE : 0);

        if (prefix == NULL)
        {
            /* Standbys and restored copies can still have valid old bytes.
             * Publish a complete image before VACUUM frees heap TIDs. */
            PageInit(image, BLCKSZ, 0);
            prefix = (CuvsCagraMeta *) PageGetContents(image);
            prefix->magic = CUVS_CAGRA_PAGE_MAGIC;
            prefix->major = CUVS_CAGRA_PAGE_MAJOR;
            set_lower(image, CAGRA_META_PREFIX_SIZE);
        }
        else
            prefix = (CuvsCagraMeta *) PageGetContents(image);
        prefix->flags |= CUVS_CAGRA_REINDEX_REQUIRED;
        GenericXLogFinish(wal);
    }
    UnlockReleaseBuffer(buf);
}

static IndexBulkDeleteResult *
vacuum_skip(IndexVacuumInfo *info, BlockNumber block, bool prefix_readable)
{
    Relation index = info->index;

    /* PG16 also calls ambulkdelete for CREATE INDEX CONCURRENTLY validation. */
    if (info->report_progress)
        bad_page(index, block);

    vacuum_mark_reindex(index, info->strategy, prefix_readable);
    UnlockPage(index, 0, ExclusiveLock);
    vacuum_warn_reindex(index);
    return NULL;
}

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

    vac->block = block;
    if (block == 0 || block >= vac->nblocks)
        return InvalidBuffer;
    if (vac->nheld == lengthof(vac->held))
        vacuum_finish_batch(vac);
    CHECK_FOR_INTERRUPTS();
    buf = ReadBufferExtended(vac->index, MAIN_FORKNUM, block, RBM_NORMAL, vac->strategy);
    LockBuffer(buf, vac->validate_only ? BUFFER_LOCK_SHARE : BUFFER_LOCK_EXCLUSIVE);
    vac->held[vac->nheld++] = buf;
    return buf;
}

static bool
vacuum_tids(CagraVacuum *vac, Buffer buf, uint32 start, uint32 count,
            const CuvsCagraTidPage *tid_tag)
{
    Page page = BufferGetPage(buf);
    const ItemPointerData *items = (ItemPointerData *) PageGetContents(page);
    Page image = NULL;
    uint32 old_dead = 0;
    uint32 removed = 0;

    for (uint32 i = start; i < count; ++i)
    {
        ItemPointerData tid = items[i];

        if (!ItemPointerIsValid(&tid))
        {
            ++old_dead;
            continue;
        }
        if (vac->validate_only)
        {
            if (!BlockNumberIsValid(ItemPointerGetBlockNumber(&tid)) ||
                ItemPointerGetOffsetNumber(&tid) > MaxHeapTuplesPerPage)
                return false;
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
        ItemPointerSetInvalid(&((ItemPointerData *) PageGetContents(image))[i]);
        ++removed;
    }

    if (tid_tag && old_dead != tid_tag->dead)
        return false;
    if (!vac->validate_only)
    {
        vac->removed += removed;
        if (tid_tag)
        {
            if (image)
                ((CuvsCagraTidPage *) PageGetSpecialPointer(image))->dead = old_dead + removed;
            vac->dead_slots += old_dead + removed;
        }
    }
    return true;
}

static bool
vacuum_pending(CagraVacuum *vac, const CuvsCagraMeta *meta)
{
    BlockNumber block = meta->pending_head;
    uint64 expected = 0;

    for (;;)
    {
        Buffer buf = vacuum_lock(vac, block);
        const CuvsCagraPending *tag;
        uint32 start = 0;
        BlockNumber next;

        if (!BufferIsValid(buf))
            return false;
        tag = pending_tag(BufferGetPage(buf), block, meta->index_nonce);
        if (tag == NULL || (expected && tag->first_pos != expected) ||
            tag->first_pos > UINT64_MAX - tag->count)
            return false;
        next = tag->next;
        /* A concurrent insert may extend the snapshotted tail. Other pages
         * must be full and link to the next position in the snapshot. */
        if (block != meta->pending_tail &&
            (!BlockNumberIsValid(next) || tag->count != CUVS_CAGRA_PENDING_PER_PAGE))
            return false;
        expected = tag->first_pos + tag->count;
        if (meta->folded > tag->first_pos)
            start = Min(meta->folded - tag->first_pos, (uint64) tag->count);
        if (!vacuum_tids(vac, buf, start, tag->count, NULL))
            return false;
        if (vac->validate_only)
            vacuum_finish_batch(vac);
        if (block == meta->pending_tail)
            return true;
        block = next;
    }
}

static bool
vacuum_slots(CagraVacuum *vac, const CuvsCagraMeta *meta)
{
    uint64 slot = 0;

    for (uint32 extent = 0; slot < meta->n_slots; ++extent)
    {
        BlockNumber base;

        vac->block = 0;
        if (extent >= meta->n_extents)
            return false;
        base = meta->extents[extent];
        if (base == 0 || base >= InvalidBlockNumber - CUVS_CAGRA_PAGE_EXTENT)
            return false;
        for (uint32 i = 0; i < CUVS_CAGRA_PAGE_EXTENT && slot < meta->n_slots; ++i)
        {
            BlockNumber block = base + i;
            uint32 count = Min((uint64) CUVS_CAGRA_TIDS_PER_PAGE, meta->n_slots - slot);
            Buffer buf = vacuum_lock(vac, block);
            Page page;
            PageHeader header;
            const CuvsCagraTidPage *tag;

            if (!BufferIsValid(buf))
                return false;
            page = BufferGetPage(buf);
            header = (PageHeader) page;
            if (!page_bounds_valid(page) || PageGetSpecialSize(page) != sizeof(*tag) ||
                header->pd_lower < MAXALIGN(SizeOfPageHeaderData) + count * sizeof(ItemPointerData))
                return false;
            tag = (CuvsCagraTidPage *) PageGetSpecialPointer(page);
            if (tag->kind != CUVS_PAGE_TIDS || tag->base_nonce != meta->base_nonce ||
                tag->first_slot != slot || tag->flags != 0 || tag->dead > count ||
                !vacuum_tids(vac, buf, 0, count, tag))
                return false;
            if (vac->validate_only)
                vacuum_finish_batch(vac);
            slot += count;
        }
    }
    return true;
}

/* Validate the snapshot before the first TID WAL batch. The page lock excludes
 * other vacuum writers; inserts can only append or extend the pending tail. */
static bool
vacuum_validate(CagraVacuum *vac, const CuvsCagraMeta *meta)
{
    MemoryContext context = CurrentMemoryContext;
    ResourceOwner caller = CurrentResourceOwner;
    ResourceOwner reader;
    bool valid = false;

    if (meta->delete_generation == UINT64_MAX)
        return false;
    vac->nblocks = RelationGetNumberOfBlocks(vac->index);
    vac->validate_only = true;
    reader = ResourceOwnerCreate(caller, "CAGRA VACUUM validation");
    PG_TRY();
    {
        CurrentResourceOwner = reader;
        valid = vacuum_pending(vac, meta) && vacuum_slots(vac, meta);
        vacuum_finish_batch(vac);
    }
    PG_CATCH();
    {
        /* The read-only pass releases each content lock before reading the
         * next block. A failed read can only leave its I/O and pin behind. */
        CurrentResourceOwner = caller;
        discard_corrupt_read(reader, context);
        valid = false;
    }
    PG_END_TRY();
    CurrentResourceOwner = caller;
    ResourceOwnerDelete(reader);
    vac->validate_only = false;
    return valid;
}

IndexBulkDeleteResult *
cuvs_cagra_page_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                           IndexBulkDeleteCallback callback, void *callback_state)
{
    Relation index = info->index;
    CuvsCagraMeta meta;
    CagraVacuum vac = {.index = index, .callback = callback, .callback_state = callback_state,
                       .strategy = info->strategy};
    Buffer mb;
    CuvsCagraMeta *updated;
    GenericXLogState *wal;

    bool prefix_readable;

    LockPage(index, 0, ExclusiveLock);
    if (!read_cagra_meta(index, info->strategy, &meta, &prefix_readable) ||
        !vacuum_validate(&vac, &meta))
        return vacuum_skip(info, vac.block, prefix_readable);
    if (!vacuum_pending(&vac, &meta) || !vacuum_slots(&vac, &meta))
        bad_page(index, vac.block);
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

    if (stats == NULL)
        stats = palloc0(sizeof(*stats));
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
    CagraVacuum vac = {.index = info->index, .strategy = info->strategy};
    bool prefix_readable;

    if (info->analyze_only)
        return stats;

    LockPage(info->index, 0, ExclusiveLock);
    if (!read_cagra_meta(info->index, info->strategy, &meta, &prefix_readable) ||
        (stats == NULL && !vacuum_validate(&vac, &meta)))
        return vacuum_skip(info, vac.block, prefix_readable);
    if (stats != NULL)
        stats->num_pages = RelationGetNumberOfBlocks(info->index);
    UnlockPage(info->index, 0, ExclusiveLock);
    return stats;
}

#endif

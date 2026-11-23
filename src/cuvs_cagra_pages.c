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
#include "access/xloginsert.h"
#include "cuvs_util.h"
#include "storage/bufpage.h"

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
                           block, RelationGetRelationName(index))));
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
        meta->minor != 0 || meta->index_nonce == 0 || meta->base_nonce == 0 ||
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

static CuvsCagraPending *
page_pending(Relation index, BlockNumber block, Page page, uint64 nonce)
{
    CuvsCagraPending *pending;
    PageHeader header = (PageHeader) page;

    if (PageIsNew(page) || PageGetSpecialSize(page) != sizeof(*pending))
        bad_page(index, block);
    pending = (CuvsCagraPending *) PageGetSpecialPointer(page);
    if (pending->kind != CUVS_PAGE_PENDING || pending->index_nonce != nonce ||
        pending->flags != 0 || pending->count > CUVS_CAGRA_PENDING_PER_PAGE ||
        header->pd_lower != MAXALIGN(SizeOfPageHeaderData) + pending->count * sizeof(ItemPointerData) ||
        pending->next == block || pending->next == 0)
        bad_page(index, block);
    return pending;
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
cuvs_cagra_append_pending(Relation index, CuvsCagraRelCache *cache,
                          ItemPointer tid, int dim)
{
    BlockNumber tail;
    uint64 next_pos = 0;

    if (cache->index_nonce == 0)
    {
        CuvsCagraMeta meta;

        if (!cuvs_cagra_read_meta(index, &meta))
            bad_page(index, 0);
        cache->tail = meta.pending_tail;
        cache->index_nonce = meta.index_nonce;
        cache->dim = meta.dim;
    }
    cuvs_cagra_check_dim(cache->dim, dim);
    tail = cache->tail;

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

        LockBuffer(tb, BUFFER_LOCK_EXCLUSIVE);
        pending = page_pending(index, tail, BufferGetPage(tb), cache->index_nonce);
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
        if (full && cache->dim == 0)
        {
            CuvsCagraMeta current;

            if (!cuvs_cagra_read_meta(index, &current) ||
                current.index_nonce != cache->index_nonce || current.pending_tail != tail)
                bad_page(index, 0);
            cuvs_cagra_check_dim(current.dim, dim);
        }
        if (full)
            nb = extend_page(index, MAIN_FORKNUM);
        if (cache->dim == 0 || full)
        {
            mb = ReadBuffer(index, 0);
            LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
            meta = page_meta(BufferGetPage(mb));
            if (meta == NULL || meta->index_nonce != cache->index_nonce ||
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
            init_pending(image, cache->index_nonce, old->first_pos + old->count);
            tail = next;
        }
        append_item(image, tid);
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
        cache->tail = tail;
        cache->dim = dim;
        return;
    }
}

#endif

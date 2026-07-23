/*
 * test_cuvs_util.c — standalone unit tests for src/cuvs_util.{h,c}.
 *
 * No framework, no PostgreSQL, no CUDA. Build + run via `make test-unit`.
 * Returns non-zero on any failure.
 */

#include "cuvs_util.h"
#include "cuvs_ipc.h"   /* CUVS_STATUS_* values */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do {                                          \
    if (cond) {                                                         \
        g_pass++;                                                       \
    } else {                                                            \
        g_fail++;                                                       \
        fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
    }                                                                   \
} while (0)

static void
test_tid_roundtrip(void)
{
    struct { uint32_t block; uint16_t offset; } cases[] = {
        { 0,          0 },
        { 0,          0xFFFF },
        { 1,          1 },
        { 0xFFFFFFFFu, 0 },
        { 0xFFFFFFFFu, 0xFFFF },
        { 123456,     7 },
        { 0x00ABCDEFu, 0x1234 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint64_t enc = cuvs_tid_encode(cases[i].block, cases[i].offset);
        uint32_t block;
        uint16_t offset;
        cuvs_tid_decode(enc, &block, &offset);
        ASSERT(block == cases[i].block, "tid block roundtrip");
        ASSERT(offset == cases[i].offset, "tid offset roundtrip");
    }

    /* Explicit bit-layout check: block<<16 | offset */
    ASSERT(cuvs_tid_encode(0xABCD, 0x1234) == (((uint64_t)0xABCD << 16) | 0x1234),
           "tid encode bit layout");
}

static void
test_parse_index_filename(void)
{
    uint32_t db = 999, ix = 999;

    ASSERT(cuvs_parse_index_filename("16384_24576.cagra", &db, &ix) == 0,
           "valid filename parses");
    ASSERT(db == 16384 && ix == 24576, "valid filename values");

    db = ix = 999;
    ASSERT(cuvs_parse_index_filename("1_2.cagra", &db, &ix) == 0, "minimal valid");
    ASSERT(db == 1 && ix == 2, "minimal valid values");

    /* Invalid inputs */
    ASSERT(cuvs_parse_index_filename(NULL, &db, &ix) == -1, "null name");
    ASSERT(cuvs_parse_index_filename("16384_24576.tids", &db, &ix) == -1,
           "wrong suffix (.tids)");
    ASSERT(cuvs_parse_index_filename(".cagra", &db, &ix) == -1, "suffix only");
    ASSERT(cuvs_parse_index_filename("16384.cagra", &db, &ix) == -1,
           "missing underscore");
    ASSERT(cuvs_parse_index_filename("notanumber_x.cagra", &db, &ix) == -1,
           "non-numeric fields");
    ASSERT(cuvs_parse_index_filename("16384_24576.cagra.bak", &db, &ix) == -1,
           "trailing extension");
    ASSERT(cuvs_parse_index_filename("", &db, &ix) == -1, "empty string");

    /* More adversarial separators / missing fields */
    ASSERT(cuvs_parse_index_filename("16384__24576.cagra", &db, &ix) == -1,
           "double underscore");
    ASSERT(cuvs_parse_index_filename("16384_.cagra", &db, &ix) == -1,
           "missing second oid");
    ASSERT(cuvs_parse_index_filename("_24576.cagra", &db, &ix) == -1,
           "missing first oid");

    /* Boundary: 0_0 is a structurally valid name (OID 0 never occurs in
     * practice, but the parser must not special-case it). */
    db = ix = 999;
    ASSERT(cuvs_parse_index_filename("0_0.cagra", &db, &ix) == 0, "zero oids parse");
    ASSERT(db == 0 && ix == 0, "zero oids values");

    /* Phase 3F: shard artifacts ("<db>_<idx>.sNNN.cagra") must NOT be parsed as
     * an unsharded index, even though they end in ".cagra". */
    ASSERT(cuvs_parse_index_filename("16384_24576.s000.cagra", &db, &ix) == -1,
           "reject shard artifact .s000.cagra");
    ASSERT(cuvs_parse_index_filename("1_2.s015.cagra", &db, &ix) == -1,
           "reject shard artifact .s015.cagra");
}

/*
 * #77: the daemon reads offsetof(CuvsCmdFrame, op) bytes, validates the magic
 * and version, and only then reads the rest. That two-stage read is what turns
 * a daemon/extension skew into a named error instead of a silently
 * misinterpreted struct — so the layout it depends on is pinned here.
 */
static void
test_proto_frame_layout(void)
{
    ASSERT(offsetof(CuvsCmdFrame, proto_magic) == 0,
           "proto_magic must be the first frame field");
    ASSERT(offsetof(CuvsCmdFrame, proto_version) == sizeof(uint32_t),
           "proto_version must directly follow proto_magic");
    ASSERT(offsetof(CuvsCmdFrame, op) == 2 * sizeof(uint32_t),
           "op must follow the 8-byte protocol prologue");

    /* Reverse-direction detection: an old daemon reads our magic as `op`, so
     * the magic must not collide with any op code it might dispatch on. */
    ASSERT(CUVS_PROTO_MAGIC > 1000u, "magic must not collide with an op code");
    ASSERT(CUVS_PROTO_VERSION >= 1u, "protocol version starts at 1");
}

static void
test_status_str(void)
{
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_PROTO_MISMATCH), "proto_mismatch") == 0,
           "status proto_mismatch");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_OK), "ok") == 0, "status ok");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_ERROR), "error") == 0, "status error");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_OOM_FALLBACK), "oom_fallback") == 0,
           "status oom_fallback");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_NOT_FOUND), "not_found") == 0,
           "status not_found");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_UNAVAILABLE), "unavailable") == 0,
           "status unavailable");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_BUILD_FAILED), "build_failed") == 0,
           "status build_failed");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_PERSIST_FAILED), "persist_failed") == 0,
           "status persist_failed");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_DIM_MISMATCH), "dim_mismatch") == 0,
           "status dim_mismatch");
    ASSERT(strcmp(cuvs_status_str(42), "unknown") == 0, "status out-of-range");
    ASSERT(strcmp(cuvs_status_str(-1), "unknown") == 0, "status negative");
}

static void
test_circuit_breaker(void)
{
    const uint32_t oid = 7;
    const int threshold = 3;

    cuvs_circuit_reset_all();
    ASSERT(cuvs_n_circuit_breakers == 0, "reset_all zeroes count");
    ASSERT(cuvs_circuit_is_open(oid) == 0, "fresh breaker is closed");

    /* Errors below threshold do not trip */
    cuvs_circuit_record_error(oid, threshold);
    cuvs_circuit_record_error(oid, threshold);
    ASSERT(cuvs_circuit_is_open(oid) == 0, "below threshold stays closed");

    /* Reaching threshold trips it open */
    cuvs_circuit_record_error(oid, threshold);
    ASSERT(cuvs_circuit_is_open(oid) == 1, "threshold trips open");

    /* record_success clears the error count but not the open flag */
    cuvs_circuit_record_success(oid);
    ASSERT(cuvs_circuit_is_open(oid) == 1, "success leaves open flag set");

    /* reset clears both */
    cuvs_circuit_reset(oid);
    ASSERT(cuvs_circuit_is_open(oid) == 0, "reset clears open flag");

    /* After reset, must accumulate threshold errors again to trip */
    cuvs_circuit_record_error(oid, threshold);
    cuvs_circuit_record_error(oid, threshold);
    ASSERT(cuvs_circuit_is_open(oid) == 0, "post-reset below threshold closed");

    /* Independent OIDs tracked separately */
    cuvs_circuit_reset_all();
    cuvs_circuit_record_error(1, threshold);
    cuvs_circuit_record_error(1, threshold);
    cuvs_circuit_record_error(1, threshold);
    ASSERT(cuvs_circuit_is_open(1) == 1, "oid 1 open");
    ASSERT(cuvs_circuit_is_open(2) == 0, "oid 2 unaffected");

    /* reset_all zeroes everything between cases */
    cuvs_circuit_reset_all();
    ASSERT(cuvs_circuit_is_open(1) == 0, "reset_all clears oid 1");
    ASSERT(cuvs_n_circuit_breakers == 0, "reset_all count back to zero");
}

static void
test_crc32(void)
{
    /* Standard CRC-32 (IEEE 802.3) check vectors. */
    ASSERT(cuvs_crc32("", 0) == 0x00000000u, "crc32 empty");
    ASSERT(cuvs_crc32("123456789", 9) == 0xCBF43926u, "crc32 check string");
    ASSERT(cuvs_crc32("a", 1) == 0xE8B7BE43u, "crc32 single byte");

    /* Sensitivity: a one-byte change must change the crc. */
    ASSERT(cuvs_crc32("123456789", 9) != cuvs_crc32("123456780", 9),
           "crc32 detects single-byte diff");
}

static void
test_tids_roundtrip(void)
{
    const int64_t  n = 5;
    const uint32_t dim = 128, metric = 1;
    uint64_t tids[5] = { 0, 1, 0xFFFFFFFFFFFFFFFFull, 42, 0xABCDEF01ull };

    FILE *f = tmpfile();
    ASSERT(f != NULL, "tmpfile open (write)");
    ASSERT(cuvs_tids_write(f, n, dim, metric, tids) == 0, "tids_write ok");
    rewind(f);

    CuvsTidsHeader hdr;
    uint64_t *out = NULL;
    ASSERT(cuvs_tids_read(f, &hdr, &out) == 0, "tids_read ok");
    ASSERT(hdr.magic == CUVS_TIDS_MAGIC, "rt magic");
    ASSERT(hdr.version == CUVS_TIDS_VERSION, "rt version");
    ASSERT(hdr.n_vecs == n, "rt n_vecs");
    ASSERT(hdr.dim == dim, "rt dim");
    ASSERT(hdr.metric == metric, "rt metric");
    ASSERT(hdr.reserved == 0, "rt reserved zero");
    if (out)
    {
        ASSERT(memcmp(out, tids, sizeof(tids)) == 0, "rt tids body identity");
        free(out);
    }
    fclose(f);
}

/* Write a valid .tids into a tmpfile, optionally let the caller corrupt the
 * raw bytes, then assert cuvs_tids_read rejects it. */
static void
test_tids_rejections(void)
{
    const int64_t  n = 4;
    const uint32_t dim = 16, metric = 0;
    uint64_t tids[4] = { 10, 20, 30, 40 };

    /* bad magic */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        rewind(f);
        uint32_t bad = 0xDEADBEEFu;
        fwrite(&bad, sizeof(bad), 1, f);   /* overwrite magic */
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject bad magic");
        ASSERT(o == NULL, "bad magic leaves out NULL");
        fclose(f);
    }

    /* bad version */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, sizeof(uint32_t), SEEK_SET);  /* version field */
        uint32_t badv = 999u;
        fwrite(&badv, sizeof(badv), 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject bad version");
        fclose(f);
    }

    /* n_vecs <= 0 */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, offsetof(CuvsTidsHeader, n_vecs), SEEK_SET);
        int64_t zero = 0;
        fwrite(&zero, sizeof(zero), 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject n_vecs<=0");
        fclose(f);
    }

    /* n_vecs > CAP */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, offsetof(CuvsTidsHeader, n_vecs), SEEK_SET);
        int64_t huge = CUVS_TIDS_MAX_VECS + 1;
        fwrite(&huge, sizeof(huge), 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject n_vecs>CAP");
        fclose(f);
    }

    /* truncated body: header claims n=4 but only 2 TIDs present */
    {
        FILE *f = tmpfile();
        CuvsTidsHeader h;
        h.magic = CUVS_TIDS_MAGIC; h.version = CUVS_TIDS_VERSION;
        h.n_vecs = n; h.dim = dim; h.metric = metric;
        h.body_crc32 = cuvs_crc32(tids, sizeof(tids)); h.reserved = 0;
        fwrite(&h, sizeof(h), 1, f);
        fwrite(tids, sizeof(uint64_t), 2, f);   /* short body */
        rewind(f);
        CuvsTidsHeader hr; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &hr, &o) == -1, "reject truncated body");
        ASSERT(o == NULL, "truncated body frees alloc");
        fclose(f);
    }

    /* corrupted body: flip a byte so crc mismatches */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, sizeof(CuvsTidsHeader), SEEK_SET);  /* first body byte */
        unsigned char b;
        fread(&b, 1, 1, f);
        fseek(f, sizeof(CuvsTidsHeader), SEEK_SET);
        b ^= 0xFFu;
        fwrite(&b, 1, 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject crc mismatch");
        ASSERT(o == NULL, "crc mismatch frees alloc");
        fclose(f);
    }

    /* reserved != 0: valid otherwise, but the reserved field is set */
    {
        FILE *f = tmpfile();
        CuvsTidsHeader h;
        h.magic = CUVS_TIDS_MAGIC; h.version = CUVS_TIDS_VERSION;
        h.n_vecs = n; h.dim = dim; h.metric = metric;
        h.body_crc32 = cuvs_crc32(tids, sizeof(tids)); h.reserved = 1;
        fwrite(&h, sizeof(h), 1, f);
        fwrite(tids, sizeof(uint64_t), (size_t)n, f);
        rewind(f);
        CuvsTidsHeader hr; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &hr, &o) == -1, "reject reserved != 0");
        ASSERT(o == NULL, "reserved!=0 leaves out NULL");
        fclose(f);
    }
}

/* ---- .vectors sidecar (Phase 3L) — mirrors the .tids tests above ---- */
static void
test_vectors_roundtrip(void)
{
    const int64_t  n = 3;
    const uint32_t dim = 4, metric = CUVS_METRIC_L2;
    /* row-major n*dim float matrix */
    float vecs[3 * 4] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 0.0f,
        0.25f, 0.25f, 0.25f, 0.25f,
    };

    FILE *f = tmpfile();
    ASSERT(f != NULL, "vectors tmpfile open (write)");
    ASSERT(cuvs_vectors_write(f, n, dim, metric, 0xCAFEF00Du, vecs) == 0, "vectors_write ok");
    rewind(f);

    CuvsVectorsHeader hdr;
    float *out = NULL;
    ASSERT(cuvs_vectors_read(f, &hdr, &out) == 0, "vectors_read ok");
    ASSERT(hdr.magic == CUVS_VECTORS_MAGIC, "rt magic");
    ASSERT(hdr.version == CUVS_VECTORS_VERSION, "rt version");
    ASSERT(hdr.n_vecs == n, "rt n_vecs");
    ASSERT(hdr.dim == dim, "rt dim");
    ASSERT(hdr.metric == metric, "rt metric");
    ASSERT(hdr.base_tids_crc32 == 0xCAFEF00Du, "rt generation token");
    ASSERT(hdr.reserved == 0, "rt reserved zero");
    if (out)
    {
        ASSERT(memcmp(out, vecs, sizeof(vecs)) == 0, "rt vectors body identity");
        free(out);
    }
    fclose(f);
}

static void
test_vectors_rejections(void)
{
    const int64_t  n = 2;
    const uint32_t dim = 4, metric = 0;
    float vecs[2 * 4] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    /* bad magic */
    {
        FILE *f = tmpfile();
        cuvs_vectors_write(f, n, dim, metric, 0, vecs);
        rewind(f);
        uint32_t bad = 0xDEADBEEFu;
        fwrite(&bad, sizeof(bad), 1, f);
        rewind(f);
        CuvsVectorsHeader h; float *o = NULL;
        ASSERT(cuvs_vectors_read(f, &h, &o) == -1, "reject bad magic");
        ASSERT(o == NULL, "bad magic leaves out NULL");
        fclose(f);
    }

    /* dim == 0 */
    {
        FILE *f = tmpfile();
        cuvs_vectors_write(f, n, dim, metric, 0, vecs);
        fseek(f, offsetof(CuvsVectorsHeader, dim), SEEK_SET);
        uint32_t zero = 0;
        fwrite(&zero, sizeof(zero), 1, f);
        rewind(f);
        CuvsVectorsHeader h; float *o = NULL;
        ASSERT(cuvs_vectors_read(f, &h, &o) == -1, "reject dim==0");
        fclose(f);
    }

    /* dim > CAP */
    {
        FILE *f = tmpfile();
        cuvs_vectors_write(f, n, dim, metric, 0, vecs);
        fseek(f, offsetof(CuvsVectorsHeader, dim), SEEK_SET);
        uint32_t huge = CUVS_VECTORS_MAX_DIM + 1;
        fwrite(&huge, sizeof(huge), 1, f);
        rewind(f);
        CuvsVectorsHeader h; float *o = NULL;
        ASSERT(cuvs_vectors_read(f, &h, &o) == -1, "reject dim>CAP");
        fclose(f);
    }

    /* truncated body: header claims n=2 but only 1 vector present */
    {
        FILE *f = tmpfile();
        CuvsVectorsHeader h;
        h.magic = CUVS_VECTORS_MAGIC; h.version = CUVS_VECTORS_VERSION;
        h.n_vecs = n; h.dim = dim; h.metric = metric;
        h.body_crc32 = cuvs_crc32(vecs, sizeof(vecs)); h.base_tids_crc32 = 0; h.reserved = 0;
        fwrite(&h, sizeof(h), 1, f);
        fwrite(vecs, sizeof(float), (size_t)dim, f);   /* short body */
        rewind(f);
        CuvsVectorsHeader hr; float *o = NULL;
        ASSERT(cuvs_vectors_read(f, &hr, &o) == -1, "reject truncated body");
        ASSERT(o == NULL, "truncated body frees alloc");
        fclose(f);
    }

    /* corrupted body: flip a byte so crc mismatches */
    {
        FILE *f = tmpfile();
        cuvs_vectors_write(f, n, dim, metric, 0, vecs);
        fseek(f, sizeof(CuvsVectorsHeader), SEEK_SET);
        unsigned char b;
        fread(&b, 1, 1, f);
        fseek(f, sizeof(CuvsVectorsHeader), SEEK_SET);
        b ^= 0xFFu;
        fwrite(&b, 1, 1, f);
        rewind(f);
        CuvsVectorsHeader h; float *o = NULL;
        ASSERT(cuvs_vectors_read(f, &h, &o) == -1, "reject crc mismatch");
        ASSERT(o == NULL, "crc mismatch frees alloc");
        fclose(f);
    }

    /* reserved != 0 */
    {
        FILE *f = tmpfile();
        CuvsVectorsHeader h;
        h.magic = CUVS_VECTORS_MAGIC; h.version = CUVS_VECTORS_VERSION;
        h.n_vecs = n; h.dim = dim; h.metric = metric;
        h.body_crc32 = cuvs_crc32(vecs, sizeof(vecs)); h.base_tids_crc32 = 0; h.reserved = 1;
        fwrite(&h, sizeof(h), 1, f);
        fwrite(vecs, sizeof(float), (size_t)n * dim, f);
        rewind(f);
        CuvsVectorsHeader hr; float *o = NULL;
        ASSERT(cuvs_vectors_read(f, &hr, &o) == -1, "reject reserved != 0");
        ASSERT(o == NULL, "reserved!=0 leaves out NULL");
        fclose(f);
    }
}

/* ---- BF micro-batch grouping (Phase 3L-9) ---- */
static void
test_bf_batch_group(void)
{
    /* 5 requests, 2 distinct keys (A and B) interleaved: A B A A B. */
    CuvsBfKey keys[5] = {
        {1, 10, 0, 8},   /* A */
        {1, 20, 0, 8},   /* B */
        {1, 10, 0, 8},   /* A */
        {1, 10, 0, 8},   /* A */
        {1, 20, 0, 8},   /* B */
    };
    int gid[5];
    int ng = -1;
    cuvs_bf_batch_group(keys, 5, gid, &ng);
    ASSERT(ng == 2, "two distinct keys -> 2 groups");
    ASSERT(gid[0] == 0, "req0 (A) -> first-seen group 0");
    ASSERT(gid[1] == 1, "req1 (B) -> first-seen group 1");
    ASSERT(gid[2] == 0 && gid[3] == 0, "later A's share group 0");
    ASSERT(gid[4] == 1, "later B shares group 1");
    ASSERT(gid[0] != gid[1], "A and B are different groups");

    /* precision distinguishes groups (same db/index/dim, different precision). */
    {
        CuvsBfKey kp[2] = { {1, 10, 0, 8}, {1, 10, 1, 8} };
        int g[2], n = -1;
        cuvs_bf_batch_group(kp, 2, g, &n);
        ASSERT(n == 2 && g[0] != g[1], "precision distinguishes groups");
    }
    /* dim distinguishes groups. */
    {
        CuvsBfKey kd[2] = { {1, 10, 0, 8}, {1, 10, 0, 16} };
        int g[2], n = -1;
        cuvs_bf_batch_group(kd, 2, g, &n);
        ASSERT(n == 2 && g[0] != g[1], "dim distinguishes groups");
    }
    /* index_oid distinguishes groups. */
    {
        CuvsBfKey ki[2] = { {1, 10, 0, 8}, {1, 11, 0, 8} };
        int g[2], n = -1;
        cuvs_bf_batch_group(ki, 2, g, &n);
        ASSERT(n == 2 && g[0] != g[1], "index_oid distinguishes groups");
    }
    /* single request -> one group. */
    {
        CuvsBfKey k1[1] = { {1, 10, 0, 8} };
        int g[1], n = -1;
        cuvs_bf_batch_group(k1, 1, g, &n);
        ASSERT(n == 1 && g[0] == 0, "single request -> one group");
    }
    /* all identical -> one group, every id 0. */
    {
        CuvsBfKey ks[4] = { {2,5,1,16}, {2,5,1,16}, {2,5,1,16}, {2,5,1,16} };
        int g[4], n = -1;
        cuvs_bf_batch_group(ks, 4, g, &n);
        ASSERT(n == 1 && g[0]==0 && g[1]==0 && g[2]==0 && g[3]==0,
               "all identical -> single group");
    }
}

/* Build a coherent 3-shard manifest covering [0,100): 40 + 35 + 25. */
static void
make_good_shards(CuvsShardRecord recs[3], uint32_t dim, uint32_t metric)
{
    int64_t sizes[3] = { 40, 35, 25 };
    int64_t off = 0;
    for (uint32_t i = 0; i < 3; i++)
    {
        recs[i].shard_id      = i;
        recs[i].gpu_device_id = i % 2;
        recs[i].tid_offset    = off;
        recs[i].n_vecs        = sizes[i];
        recs[i].dim           = dim;
        recs[i].metric        = metric;
        recs[i].artifact_crc32 = 0xA5A50000u + i;
        recs[i].reserved      = 0;
        off += sizes[i];
    }
}

static void
test_shards_roundtrip(void)
{
    const uint32_t dim = 128, metric = CUVS_METRIC_L2;
    const int64_t  n = 100;
    CuvsShardRecord recs[3];
    make_good_shards(recs, dim, metric);

    FILE *f = tmpfile();
    ASSERT(f != NULL, "shards tmpfile open");
    ASSERT(cuvs_shards_write(f, 3, n, dim, metric, 0xDEADBEEFu, recs) == 0,
           "shards_write ok");
    rewind(f);

    CuvsShardsHeader hdr;
    CuvsShardRecord *out = NULL;
    ASSERT(cuvs_shards_read(f, &hdr, &out) == 0, "shards_read ok");
    ASSERT(hdr.magic == CUVS_SHARDS_MAGIC, "rt shards magic");
    ASSERT(hdr.version == CUVS_SHARDS_VERSION, "rt shards version");
    ASSERT(hdr.shard_count == 3, "rt shard_count");
    ASSERT(hdr.base_tids_crc32 == 0xDEADBEEFu, "rt base generation token");
    ASSERT(hdr.n_vecs == n, "rt n_vecs");
    ASSERT(hdr.dim == dim, "rt dim");
    ASSERT(hdr.metric == metric, "rt metric");
    ASSERT(hdr.reserved == 0, "rt shards reserved zero");
    if (out)
    {
        ASSERT(out[1].tid_offset == 40, "rt shard1 offset");
        ASSERT(out[2].tid_offset == 75, "rt shard2 offset");
        ASSERT(out[2].n_vecs == 25, "rt shard2 n_vecs");
        ASSERT(memcmp(out, recs, sizeof(recs)) == 0, "rt shard records identity");
        free(out);
    }
    fclose(f);
}

static void
test_shards_rejections(void)
{
    const uint32_t dim = 16, metric = CUVS_METRIC_L2;
    const int64_t  n = 100;
    CuvsShardRecord recs[3];

    /* bad magic */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        cuvs_shards_write(f, 3, n, dim, metric, 0, recs);
        rewind(f);
        uint32_t bad = 0xDEADBEEFu;
        fwrite(&bad, sizeof(bad), 1, f);
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject bad magic");
        ASSERT(o == NULL, "bad magic leaves out NULL");
        fclose(f);
    }

    /* bad version */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        cuvs_shards_write(f, 3, n, dim, metric, 0, recs);
        fseek(f, offsetof(CuvsShardsHeader, version), SEEK_SET);
        uint32_t badv = 999u;
        fwrite(&badv, sizeof(badv), 1, f);
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject bad version");
        fclose(f);
    }

    /* shard_count == 0 */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        cuvs_shards_write(f, 3, n, dim, metric, 0, recs);
        fseek(f, offsetof(CuvsShardsHeader, shard_count), SEEK_SET);
        uint32_t zero = 0;
        fwrite(&zero, sizeof(zero), 1, f);
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject shard_count==0");
        fclose(f);
    }

    /* shard_count > MAX */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        cuvs_shards_write(f, 3, n, dim, metric, 0, recs);
        fseek(f, offsetof(CuvsShardsHeader, shard_count), SEEK_SET);
        uint32_t huge = CUVS_SHARDS_MAX + 1;
        fwrite(&huge, sizeof(huge), 1, f);
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject shard_count>MAX");
        fclose(f);
    }

    /* corrupted record body: flip a byte so body crc mismatches */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        cuvs_shards_write(f, 3, n, dim, metric, 0, recs);
        fseek(f, sizeof(CuvsShardsHeader), SEEK_SET);  /* first record byte */
        unsigned char b;
        fread(&b, 1, 1, f);
        fseek(f, sizeof(CuvsShardsHeader), SEEK_SET);
        b ^= 0xFFu;
        fwrite(&b, 1, 1, f);
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject body crc mismatch");
        ASSERT(o == NULL, "crc mismatch frees alloc");
        fclose(f);
    }

    /* semantic: non-contiguous offsets (gap) — body crc still valid */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        recs[1].tid_offset = 41;        /* should be 40 -> gap */
        cuvs_shards_write(f, 3, n, dim, metric, 0, recs);
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject non-contiguous offset");
        ASSERT(o == NULL, "non-contiguous frees alloc");
        fclose(f);
    }

    /* semantic: per-shard n_vecs sum != header n_vecs */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        cuvs_shards_write(f, 3, n + 1, dim, metric, 0, recs);  /* sum=100, hdr=101 */
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject sum != header n_vecs");
        fclose(f);
    }

    /* semantic: out-of-order shard_id */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        recs[0].shard_id = 1; recs[1].shard_id = 0;  /* swapped ids, offsets stay */
        cuvs_shards_write(f, 3, n, dim, metric, 0, recs);
        rewind(f);
        CuvsShardsHeader h; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &h, &o) == -1, "reject out-of-order shard_id");
        fclose(f);
    }

    /* truncated record body: header claims 3 shards but only 1 present */
    {
        FILE *f = tmpfile();
        make_good_shards(recs, dim, metric);
        CuvsShardsHeader h;
        h.magic = CUVS_SHARDS_MAGIC; h.version = CUVS_SHARDS_VERSION;
        h.shard_count = 3; h.base_tids_crc32 = 0; h.n_vecs = n;
        h.dim = dim; h.metric = metric;
        h.body_crc32 = cuvs_crc32(recs, 3 * sizeof(CuvsShardRecord));
        h.reserved = 0;
        fwrite(&h, sizeof(h), 1, f);
        fwrite(recs, sizeof(CuvsShardRecord), 1, f);   /* short body */
        rewind(f);
        CuvsShardsHeader hr; CuvsShardRecord *o = NULL;
        ASSERT(cuvs_shards_read(f, &hr, &o) == -1, "reject truncated record body");
        ASSERT(o == NULL, "truncated body frees alloc");
        fclose(f);
    }
}

#ifdef CUVS_TEST_HOOKS
static void
test_fault_hook(void)
{
    unsetenv("CUVS_FAULT_DUMMY");
    ASSERT(cuvs_fault("CUVS_FAULT_DUMMY") == 0, "fault unset returns 0");
    setenv("CUVS_FAULT_DUMMY", "1", 1);
    ASSERT(cuvs_fault("CUVS_FAULT_DUMMY") == 1, "fault set returns 1");
    unsetenv("CUVS_FAULT_DUMMY");
}
#endif

/* Write a delta header + body into a tmpfile and assert validate accepts the
 * good case and rejects bad magic/version/reserved/dim and a truncated body. */
static void
test_delta_format(void)
{
    const uint32_t dim = 4, metric = CUVS_METRIC_L2, base_crc = 0xCAFEBABEu;
    const int64_t  n = 3;
    size_t rec = cuvs_delta_record_bytes(dim);

    ASSERT(rec == sizeof(uint64_t) + 4 * sizeof(float), "delta record bytes");
    ASSERT(sizeof(CuvsDeltaHeader) == 32, "delta header is 32 bytes");

    /* init + round-trip a header through a FILE*. */
    CuvsDeltaHeader h;
    cuvs_delta_header_init(&h, dim, metric, base_crc);
    h.n_rows = n;
    ASSERT(h.magic == CUVS_DELTA_MAGIC, "init magic");
    ASSERT(h.version == CUVS_DELTA_VERSION, "init version");
    ASSERT(h.reserved == 0, "init reserved zero");
    ASSERT(h.base_tids_crc32 == base_crc, "init generation token");

    FILE *f = tmpfile();
    ASSERT(f != NULL, "delta tmpfile open");
    fwrite(&h, sizeof(h), 1, f);
    /* n records of {tid, vec[dim]} — content does not matter for validate. */
    for (int64_t i = 0; i < n; i++)
    {
        uint64_t tid = cuvs_tid_encode((uint32_t) i, (uint16_t) i);
        float    vec[4] = { (float) i, 0.0f, 0.0f, 0.0f };
        fwrite(&tid, sizeof(tid), 1, f);
        fwrite(vec, sizeof(float), dim, f);
    }
    rewind(f);

    CuvsDeltaHeader hr;
    ASSERT(cuvs_delta_read_header(f, &hr) == 0, "delta read_header ok");
    ASSERT(hr.n_rows == n && hr.dim == dim, "delta header round-trip");
    ASSERT(cuvs_delta_validate(&hr, (int64_t) (n * (int64_t) rec)) == 0,
           "validate accepts exact body size");
    ASSERT(cuvs_delta_validate(&hr, (int64_t) (n * (int64_t) rec) - 1) == -1,
           "validate rejects truncated body");
    ASSERT(cuvs_delta_validate(&hr, (int64_t) (n * (int64_t) rec) + rec) == -1,
           "validate rejects oversized body");
    fclose(f);

    /* field rejections */
    CuvsDeltaHeader b;
    cuvs_delta_header_init(&b, dim, metric, base_crc); b.n_rows = n;
    b.magic = 0xDEADBEEFu;
    ASSERT(cuvs_delta_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject bad magic");
    cuvs_delta_header_init(&b, dim, metric, base_crc); b.n_rows = n;
    b.version = 99u;
    ASSERT(cuvs_delta_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject bad version");
    cuvs_delta_header_init(&b, dim, metric, base_crc); b.n_rows = n;
    b.reserved = 1u;
    ASSERT(cuvs_delta_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject reserved != 0");
    cuvs_delta_header_init(&b, 0, metric, base_crc); b.n_rows = n;
    ASSERT(cuvs_delta_validate(&b, 0) == -1, "reject dim 0");

    /* generation mismatch is a header-field comparison the caller makes; here
     * we just confirm the token survives a round-trip so the gate can compare. */
    ASSERT(hr.base_tids_crc32 == base_crc, "generation token preserved on read");
}

/* Mirror of test_delta_format for the .tombstone sidecar (Phase 3A-4): round-trip
 * a header through a FILE* and assert validate accepts the exact body and rejects
 * a truncated/oversized body, bad magic/version/reserved, and a negative entry
 * count. Records are fixed 16 bytes ({tid, delete_xid}); there is no dim field. */
static void
test_tombstone_format(void)
{
    const uint32_t base_crc = 0xFEEDFACEu;
    const int64_t  n   = 3;
    const size_t   rec = sizeof(CuvsTombstoneRecord);

    ASSERT(rec == 2 * sizeof(uint64_t), "tombstone record is 16 bytes");
    ASSERT(sizeof(CuvsTombstoneHeader) == 32, "tombstone header is 32 bytes");

    /* init + round-trip a header through a FILE*. */
    CuvsTombstoneHeader h;
    cuvs_tombstone_header_init(&h, base_crc);
    h.n_entries = n;
    ASSERT(h.magic == CUVS_TOMBSTONE_MAGIC, "init magic");
    ASSERT(h.version == CUVS_TOMBSTONE_VERSION, "init version");
    ASSERT(h.reserved == 0, "init reserved zero");
    ASSERT(h.base_tids_crc32 == base_crc, "init generation token");

    FILE *f = tmpfile();
    ASSERT(f != NULL, "tombstone tmpfile open");
    fwrite(&h, sizeof(h), 1, f);
    /* n records of {tid, delete_xid} — content does not matter for validate. */
    for (int64_t i = 0; i < n; i++)
    {
        CuvsTombstoneRecord r;
        r.tid        = cuvs_tid_encode((uint32_t) i, (uint16_t) i);
        r.delete_xid = (uint64_t) (1000 + i);
        fwrite(&r, sizeof(r), 1, f);
    }
    rewind(f);

    CuvsTombstoneHeader hr;
    ASSERT(cuvs_tombstone_read_header(f, &hr) == 0, "tombstone read_header ok");
    ASSERT(hr.n_entries == n, "tombstone header round-trip");
    ASSERT(cuvs_tombstone_validate(&hr, (int64_t) (n * (int64_t) rec)) == 0,
           "validate accepts exact body size");
    ASSERT(cuvs_tombstone_validate(&hr, (int64_t) (n * (int64_t) rec) - 1) == -1,
           "validate rejects truncated body");
    ASSERT(cuvs_tombstone_validate(&hr, (int64_t) (n * (int64_t) rec) + rec) == -1,
           "validate rejects oversized body");
    fclose(f);

    /* field rejections */
    CuvsTombstoneHeader b;
    cuvs_tombstone_header_init(&b, base_crc); b.n_entries = n;
    b.magic = 0xDEADBEEFu;
    ASSERT(cuvs_tombstone_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject bad magic");
    cuvs_tombstone_header_init(&b, base_crc); b.n_entries = n;
    b.version = 99u;
    ASSERT(cuvs_tombstone_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject bad version");
    cuvs_tombstone_header_init(&b, base_crc); b.n_entries = n;
    b.reserved = 1u;
    ASSERT(cuvs_tombstone_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject reserved != 0");
    cuvs_tombstone_header_init(&b, base_crc); b.n_entries = -1;
    ASSERT(cuvs_tombstone_validate(&b, 0) == -1, "reject negative n_entries");

    /* generation token survives a round-trip so the gate can compare it. */
    ASSERT(hr.base_tids_crc32 == base_crc, "generation token preserved on read");
}

static void
test_lat_histogram(void)
{
    /* Bucket boundaries: 0->0, [2^(k-1),2^k)->k. */
    ASSERT(cuvs_lat_bucket_index(0) == 0, "bucket 0 us -> 0");
    ASSERT(cuvs_lat_bucket_index(1) == 1, "bucket 1 us -> 1");
    ASSERT(cuvs_lat_bucket_index(2) == 2, "bucket 2 us -> 2");
    ASSERT(cuvs_lat_bucket_index(3) == 2, "bucket 3 us -> 2 ([2,4))");
    ASSERT(cuvs_lat_bucket_index(4) == 3, "bucket 4 us -> 3");
    ASSERT(cuvs_lat_bucket_index(1000) == 10, "bucket 1000 us -> 10 ([512,1024))");
    /* Huge latency clamps to the last bucket, never overruns the array. */
    ASSERT(cuvs_lat_bucket_index(0xFFFFFFFFu) == CUVS_LAT_BUCKETS - 1,
           "bucket UINT32_MAX clamps to last");

    uint32_t hist[CUVS_LAT_BUCKETS];

    /* Empty histogram -> 0 for every quantile. */
    memset(hist, 0, sizeof(hist));
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 0, "empty p50 -> 0");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 0, "empty p99 -> 0");

    /* Single sample in bucket 5 -> upper edge 2^5 = 32. */
    memset(hist, 0, sizeof(hist));
    hist[5] = 1;
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 32u, "single p50 -> 32");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 32u, "single p99 -> 32");

    /* 100 samples all in bucket 10 -> every quantile is the bucket upper (1024). */
    memset(hist, 0, sizeof(hist));
    hist[10] = 100;
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 1024u, "uniform p50 -> 1024");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.95) == 1024u, "uniform p95 -> 1024");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 1024u, "uniform p99 -> 1024");

    /* Skewed: 90 fast (bucket 3 -> 8us), 10 slow (bucket 10 -> 1024us).
     * p50 falls in the fast bulk; p95/p99 cross into the slow tail. */
    memset(hist, 0, sizeof(hist));
    hist[3] = 90;
    hist[10] = 10;
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 8u, "skewed p50 -> 8");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.95) == 1024u, "skewed p95 -> 1024 (tail)");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 1024u, "skewed p99 -> 1024 (tail)");
}

static void
test_metric_from_opclass(void)
{
    ASSERT(cuvs_metric_from_opclass_name("vector_l2_ops")     == CUVS_METRIC_L2,     "l2 opclass -> L2");
    ASSERT(cuvs_metric_from_opclass_name("vector_cosine_ops") == CUVS_METRIC_COSINE, "cosine opclass -> COSINE");
    ASSERT(cuvs_metric_from_opclass_name("vector_ip_ops")     == CUVS_METRIC_IP,     "ip opclass -> IP");
    ASSERT(cuvs_metric_from_opclass_name("bogus_ops")         == -1, "unknown opclass -> -1");
    ASSERT(cuvs_metric_from_opclass_name("")                  == -1, "empty opclass -> -1");
    ASSERT(cuvs_metric_from_opclass_name(NULL)                == -1, "NULL opclass -> -1");
    /* status string for the new metric-mismatch code. */
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_METRIC_MISMATCH), "metric_mismatch") == 0,
           "status_str METRIC_MISMATCH");
}

static void
test_auto_shard_count(void)
{
    /* dim 16 => 16*sizeof(float) + 16*4 = 128 bytes/vector (mirrors
     * estimate_vram_bytes). needed(1000,16) = 128000 bytes. */
    const size_t GB = (size_t) 1 << 30;

    /* Fits one GPU -> unsharded. */
    ASSERT(cuvs_auto_shard_count(1000, 16, GB, 4, 256) == 1, "fits one GPU -> 1");
    /* Unlimited/unknown budget -> never auto-shard. */
    ASSERT(cuvs_auto_shard_count(1000000, 16, 0, 4, 256) == 1, "budget 0 -> 1");
    /* Degenerate inputs -> 1 (treat as unsharded, don't crash). */
    ASSERT(cuvs_auto_shard_count(0, 16, GB, 4, 256) == 1, "0 vecs -> 1");
    ASSERT(cuvs_auto_shard_count(1000, 0, GB, 4, 256) == 1, "0 dim -> 1");
    ASSERT(cuvs_auto_shard_count(1000, 16, GB, 0, 256) == 1, "0 gpus -> 1");

    /* Over single-GPU budget -> split. */
    ASSERT(cuvs_auto_shard_count(1000, 16, 64000, 4, 256) == 2, "needs 2 shards");
    ASSERT(cuvs_auto_shard_count(1000, 16, 32000, 4, 256) == 4, "needs 4 shards, 4 gpus -> 4");
    ASSERT(cuvs_auto_shard_count(1000, 16, 32000, 8, 256) == 4, "needs 4, 8 gpus -> 4");
    /* Not enough GPUs to fit -> fail closed (0). */
    ASSERT(cuvs_auto_shard_count(1000, 16, 32000, 2, 256) == 0, "needs 4 but 2 gpus -> 0");

    /* max_shards cap below requirement -> fail closed; generous cap -> full split. */
    ASSERT(cuvs_auto_shard_count(1000, 16, 1000, 256, 4)   == 0,   "needs 128 but max 4 -> 0");
    ASSERT(cuvs_auto_shard_count(1000, 16, 1000, 256, 256) == 128, "needs 128, cap 256 -> 128");

    /* >=2 vectors/shard floor: a 2-vec corpus can't split, so an unmeetable
     * budget fails closed rather than producing 1-vector shards. */
    ASSERT(cuvs_auto_shard_count(2, 16, 100, 4, 256) == 0, "2 vecs cannot split -> 0");
}

/* ADR-059: cuvs_vectors_write_multi must produce byte-identical output to
 * cuvs_vectors_write over the logically concatenated corpus — this is the
 * sidecar half of the multi-partial build correctness invariant (the GPU build
 * half is the VM self-NN single==parallel check). */
static void
test_vectors_write_multi(void)
{
    const uint32_t dim = 4, metric = CUVS_METRIC_L2, gen = 0xABCD1234u;
    /* 6 vectors, row-major. */
    float vecs[6 * 4] = {
        1,0,0,0,   0,1,0,0,   0,0,1,0,
        0,0,0,1,   2,2,2,2,   3,1,4,1,
    };

    /* (1) [2,1,3] partitioning == single 6-vec corpus, byte-for-byte. */
    {
        const int64_t n_each[3] = { 2, 1, 3 };
        const float  *parts[3]  = { &vecs[0], &vecs[2 * 4], &vecs[3 * 4] };
        unsigned char *bs = NULL, *bm = NULL;
        long ns = 0, nm = 0;
        FILE *f;

        f = tmpfile(); ASSERT(f != NULL, "wm: single tmpfile");
        ASSERT(cuvs_vectors_write(f, 6, dim, metric, gen, vecs) == 0, "wm: single write");
        fflush(f); ns = ftell(f); rewind(f);
        bs = malloc(ns); ASSERT(fread(bs, 1, ns, f) == (size_t)ns, "wm: single readback"); fclose(f);

        f = tmpfile(); ASSERT(f != NULL, "wm: multi tmpfile");
        ASSERT(cuvs_vectors_write_multi(f, n_each, 3, dim, metric, gen, parts) == 0, "wm: multi write");
        fflush(f); nm = ftell(f); rewind(f);
        bm = malloc(nm); ASSERT(fread(bm, 1, nm, f) == (size_t)nm, "wm: multi readback"); fclose(f);

        ASSERT(ns == nm, "wm: same byte length");
        if (bs && bm && ns == nm)
            ASSERT(memcmp(bs, bm, ns) == 0, "wm: multi-partition == single corpus (byte-identical)");
        free(bs); free(bm);
    }

    /* (2) empty partition (n_each==0, ptr may be NULL) is skipped: [2,0,4] == single 6. */
    {
        const int64_t n_each[3] = { 2, 0, 4 };
        const float  *parts[3]  = { &vecs[0], NULL, &vecs[2 * 4] };
        unsigned char *bm = NULL, *bs = NULL;
        long nm = 0, ns = 0;
        FILE *f;

        f = tmpfile(); ASSERT(f != NULL, "wm: empty-part tmpfile");
        ASSERT(cuvs_vectors_write_multi(f, n_each, 3, dim, metric, gen, parts) == 0, "wm: empty-part write");
        fflush(f); nm = ftell(f); rewind(f);
        bm = malloc(nm); ASSERT(fread(bm, 1, nm, f) == (size_t)nm, "wm: empty-part readback"); fclose(f);

        f = tmpfile(); cuvs_vectors_write(f, 6, dim, metric, gen, vecs);
        fflush(f); ns = ftell(f); rewind(f);
        bs = malloc(ns); ASSERT(fread(bs, 1, ns, f) == (size_t)ns, "wm: ref readback"); fclose(f);

        ASSERT(nm == ns && bm && bs && memcmp(bm, bs, nm) == 0, "wm: empty partition skipped == single");
        free(bm); free(bs);
    }

    /* (3) a multi-written file reads back with body == concatenation. */
    {
        const int64_t n_each[2] = { 4, 2 };
        const float  *parts[2]  = { &vecs[0], &vecs[4 * 4] };
        CuvsVectorsHeader h;
        float *o = NULL;
        FILE *f = tmpfile();
        cuvs_vectors_write_multi(f, n_each, 2, dim, metric, gen, parts);
        rewind(f);
        ASSERT(cuvs_vectors_read(f, &h, &o) == 0, "wm: read multi-written ok");
        ASSERT(h.n_vecs == 6, "wm: read n_vecs == 6");
        if (o) { ASSERT(memcmp(o, vecs, sizeof(vecs)) == 0, "wm: read body == concatenation"); free(o); }
        fclose(f);
    }
}

int
main(void)
{
    test_tid_roundtrip();
    test_parse_index_filename();
    test_proto_frame_layout();
    test_status_str();
    test_circuit_breaker();
    test_crc32();
    test_tids_roundtrip();
    test_tids_rejections();
    test_vectors_roundtrip();
    test_vectors_write_multi();
    test_vectors_rejections();
    test_bf_batch_group();
    test_shards_roundtrip();
    test_shards_rejections();
    test_auto_shard_count();
    test_delta_format();
    test_tombstone_format();
    test_lat_histogram();
    test_metric_from_opclass();
#ifdef CUVS_TEST_HOOKS
    test_fault_hook();
#endif

    printf("[INFO] cuvs_util unit tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("[OK] all tests passed\n");
    return g_fail == 0 ? 0 : 1;
}

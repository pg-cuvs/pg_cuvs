/* #238: remembered brute_force unavailable verdict (cuvs_bf_verdict.h).
 * refresh_main_bf_cache reads `.vectors` only when cuvs_bf_verdict_unavailable
 * returns 0. Checks: the verdict answers for the same file; an extend (row
 * count change) does not make it read the file again; another file identity
 * re-decides; a remembered header that now fits is read; a truncated file is
 * defective content; resource and I/O failures of cuvs_vectors_read are not
 * remembered. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cuvs_bf_verdict.h"
#include "cuvs_util.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

enum { INO = 77, SIZE = 4036, DIM = 8, METRIC = 0 };
static const int64_t  MTIME = 1763860000123456789LL;
static const uint32_t OLD_TIDS = 0x1111u, NEW_TIDS = 0x2222u;

static CuvsVectorsHeader
header(int64_t n, uint32_t base_tids)
{
    CuvsVectorsHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = CUVS_VECTORS_MAGIC; h.version = CUVS_VECTORS_VERSION;
    h.n_vecs = n; h.dim = DIM; h.metric = METRIC; h.base_tids_crc32 = base_tids;
    return h;
}

static void
verdict_decisions(void)
{
    CuvsBfVerdict v;
    CuvsVectorsHeader h;
    memset(&v, 0, sizeof(v));

    /* No verdict: read. */
    CHECK(!cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM, METRIC, 1, OLD_TIDS));

    /* Sidecar of 100 rows built on OLD_TIDS; the entry has 120 rows and a
     * saved .tids (NEW_TIDS): generation mismatch, remembered. */
    h = header(100, OLD_TIDS);
    CHECK(cuvs_bf_verdict_note(&v, 0, &h, INO, SIZE, MTIME) == 1);
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 120, DIM, METRIC, 1, NEW_TIDS));

    /* Extends grow the row count; the same file is not read again. */
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 121, DIM, METRIC, 1, NEW_TIDS));
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 5000, DIM, METRIC, 1, NEW_TIDS));
    /* Row count back to the file's, .tids still newer: still unavailable. */
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM, METRIC, 1, NEW_TIDS));
    /* .tids unreadable: unavailable, as on the read path. */
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM, METRIC, 0, 0));
    /* Shape differs from the entry: unavailable. */
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM + 1, METRIC, 1, OLD_TIDS));
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM, METRIC + 1, 1, OLD_TIDS));

    /* The remembered header fits the current entry and .tids: read the file. */
    CHECK(!cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM, METRIC, 1, OLD_TIDS));

    /* Another file (REINDEX writes a new sidecar): read. */
    CHECK(!cuvs_bf_verdict_unavailable(&v, INO + 1, SIZE, MTIME, 120, DIM, METRIC, 1, NEW_TIDS));
    CHECK(!cuvs_bf_verdict_unavailable(&v, INO, SIZE + 4, MTIME, 120, DIM, METRIC, 1, NEW_TIDS));
    CHECK(!cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME + 1, 120, DIM, METRIC, 1, NEW_TIDS));

    /* Defective content: unavailable for that file whatever the entry is. */
    CHECK(cuvs_bf_verdict_note(&v, CUVS_VECTORS_READ_BAD, NULL, INO, SIZE, MTIME) == 1);
    CHECK(v.content_bad);
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM, METRIC, 1, OLD_TIDS));
    CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 120, DIM, METRIC, 1, NEW_TIDS));
    CHECK(!cuvs_bf_verdict_unavailable(&v, INO + 1, SIZE, MTIME, 100, DIM, METRIC, 1, OLD_TIDS));

    /* A resource / I/O failure is not remembered: no verdict -> read. */
    memset(&v, 0, sizeof(v));
    CHECK(cuvs_bf_verdict_note(&v, CUVS_VECTORS_READ_IO, NULL, INO, SIZE, MTIME) == 0);
    CHECK(!v.valid);
    CHECK(!cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 100, DIM, METRIC, 1, OLD_TIDS));

    /* ... and it leaves an earlier verdict for another file as it was. */
    h = header(100, OLD_TIDS);
    CHECK(cuvs_bf_verdict_note(&v, 0, &h, INO, SIZE, MTIME) == 1);
    CHECK(cuvs_bf_verdict_note(&v, CUVS_VECTORS_READ_IO, NULL, INO + 1, SIZE, MTIME) == 0);
    CHECK(v.valid && v.ino == INO && !v.content_bad && v.n_vecs == 100);
}

/* The failure classes the verdict relies on, from real files. */
static void
read_failure_classes(void)
{
    float vecs[4 * DIM];
    CuvsVectorsHeader h;
    float *o;
    FILE *f;

    for (int i = 0; i < 4 * DIM; i++)
        vecs[i] = (float) i;

    /* Body crc mismatch: content defect. */
    f = tmpfile();
    CHECK(cuvs_vectors_write(f, 4, DIM, METRIC, 0, vecs) == 0);
    fseek(f, (long) sizeof(CuvsVectorsHeader), SEEK_SET);
    {
        unsigned char b = 0xA5u;
        fwrite(&b, 1, 1, f);
    }
    rewind(f);
    o = NULL;
    CHECK(cuvs_vectors_read(f, &h, &o) == CUVS_VECTORS_READ_BAD);
    CHECK(o == NULL);
    fclose(f);

    /* Bad magic: content defect. */
    f = tmpfile();
    h = header(4, 0);
    h.magic = 0;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(vecs, sizeof(float), 4 * DIM, f);
    rewind(f);
    o = NULL;
    CHECK(cuvs_vectors_read(f, &h, &o) == CUVS_VECTORS_READ_BAD);
    fclose(f);

    /* Short header: I/O. */
    f = tmpfile();
    fwrite("xy", 1, 2, f);
    rewind(f);
    o = NULL;
    CHECK(cuvs_vectors_read(f, &h, &o) == CUVS_VECTORS_READ_IO);
    fclose(f);

    /* File shorter than its header says (truncated sidecar): content defect,
     * so the verdict remembers it and the same file is not read again. */
    f = tmpfile();
    h = header(4, 0);
    h.body_crc32 = cuvs_crc32(vecs, sizeof(vecs));
    fwrite(&h, sizeof(h), 1, f);
    fwrite(vecs, sizeof(float), DIM, f);
    rewind(f);
    o = NULL;
    CHECK(cuvs_vectors_read(f, &h, &o) == CUVS_VECTORS_READ_BAD);
    CHECK(o == NULL);
    fclose(f);
    {
        CuvsBfVerdict v;
        memset(&v, 0, sizeof(v));
        CHECK(cuvs_bf_verdict_note(&v, CUVS_VECTORS_READ_BAD, NULL, INO, SIZE, MTIME) == 1);
        CHECK(cuvs_bf_verdict_unavailable(&v, INO, SIZE, MTIME, 4, DIM, METRIC, 1, OLD_TIDS));
    }

    /* One float short of the header's body: still truncated. */
    f = tmpfile();
    h = header(4, 0);
    h.body_crc32 = cuvs_crc32(vecs, sizeof(vecs));
    fwrite(&h, sizeof(h), 1, f);
    fwrite(vecs, sizeof(float), 4 * DIM - 1, f);
    rewind(f);
    o = NULL;
    CHECK(cuvs_vectors_read(f, &h, &o) == CUVS_VECTORS_READ_BAD);
    fclose(f);

    /* The same short body on a stream whose size is unknown (a pipe): the
     * read comes up short, an I/O failure, not remembered. */
    {
        int fds[2];
        CHECK(pipe(fds) == 0);
        h = header(4, 0);
        h.body_crc32 = cuvs_crc32(vecs, sizeof(vecs));
        CHECK(write(fds[1], &h, sizeof(h)) == (ssize_t) sizeof(h));
        CHECK(write(fds[1], vecs, sizeof(float) * DIM) == (ssize_t) (sizeof(float) * DIM));
        close(fds[1]);
        f = fdopen(fds[0], "rb");
        CHECK(f != NULL);
        if (f) {
            o = NULL;
            CHECK(cuvs_vectors_read(f, &h, &o) == CUVS_VECTORS_READ_IO);
            CHECK(o == NULL);
            fclose(f);
        }
    }

    /* A valid file reads. */
    f = tmpfile();
    CHECK(cuvs_vectors_write(f, 4, DIM, METRIC, OLD_TIDS, vecs) == 0);
    rewind(f);
    o = NULL;
    CHECK(cuvs_vectors_read(f, &h, &o) == 0);
    CHECK(o != NULL && h.n_vecs == 4 && h.base_tids_crc32 == OLD_TIDS);
    free(o);
    fclose(f);
}

int main(void)
{
    verdict_decisions();
    read_failure_classes();
    if (failures) {
        fprintf(stderr, "test_bf_verdict: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS bf verdict: same file answers without a read across extends; "
           "new file or fitting header reads; truncated file is content; "
           "I/O failures not remembered\n");
    return 0;
}

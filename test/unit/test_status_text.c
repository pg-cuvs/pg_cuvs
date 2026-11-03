/*
 * CPU unit tests for src/cuvs_status_text.c (I3-B status text parsing,
 * row visibility, busy-message class, RECLAIM_STATUS JSON body). No daemon,
 * no PG, no CUDA. Expected values are written out by hand.
 *
 *   make test-status-text
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cuvs_ipc.h"
#include "cuvs_status_text.h"

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                         \
    if (cond) g_pass++;                                               \
    else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
           printf(__VA_ARGS__); printf("\n"); }                       \
} while (0)

/* Count '\t'-separated names in a *_COLS header. */
static int
header_ncols(const char *h)
{
    int n = 1;
    for (; *h; h++)
        n += *h == '\t';
    return n;
}

static void
test_layout_constants(void)
{
    CHECK(strlen(CUVS_INDEX_STATUS_KINDS) == CUVS_INDEX_STATUS_NCOLS,
          "index kinds %zu != %d", strlen(CUVS_INDEX_STATUS_KINDS),
          CUVS_INDEX_STATUS_NCOLS);
    CHECK(strlen(CUVS_MEMORY_STATUS_KINDS) == CUVS_MEMORY_STATUS_NCOLS,
          "memory kinds %zu != %d", strlen(CUVS_MEMORY_STATUS_KINDS),
          CUVS_MEMORY_STATUS_NCOLS);
    CHECK(header_ncols(CUVS_INDEX_STATUS_COLS) == CUVS_INDEX_STATUS_NCOLS,
          "index header ncols");
    CHECK(header_ncols(CUVS_MEMORY_STATUS_COLS) == CUVS_MEMORY_STATUS_NCOLS,
          "memory header ncols");
}

static void
test_header(void)
{
    const char *h = "a\tb\tc";
    size_t pos = 99;

    CHECK(cuvs_status_check_header("a\tb\tc\n1\t2\t3\n", 12, h, &pos) == 1 && pos == 6,
          "exact header accepted, pos after it (pos=%zu)", pos);
    pos = 99;
    CHECK(cuvs_status_check_header("a\tb\tc\n", 6, h, &pos) == 1 && pos == 6,
          "header-only reply (zero rows) accepted");
    CHECK(cuvs_status_check_header("a\tb\tx\n", 6, h, &pos) == 0, "renamed column rejected");
    CHECK(cuvs_status_check_header("a\tb\n", 4, h, &pos) == 0, "missing column rejected");
    CHECK(cuvs_status_check_header("a\tb\tc\td\n", 8, h, &pos) == 0, "extra column rejected");
    CHECK(cuvs_status_check_header("b\ta\tc\n", 6, h, &pos) == 0, "reordered columns rejected");
    CHECK(cuvs_status_check_header("a\tb\tc", 5, h, &pos) == 0, "unterminated header rejected");
    CHECK(cuvs_status_check_header("", 0, h, &pos) == 0, "empty text rejected");
    CHECK(cuvs_status_check_header("1\t2\t3\n", 6, h, &pos) == 0, "data row as header rejected");
}

static void
test_next_line(void)
{
    const char *t = "x\ny\n";
    size_t pos = 0, n = 0;
    const char *line = NULL;

    CHECK(cuvs_status_next_line(t, 4, &pos, &line, &n) == 1 && n == 1 && line[0] == 'x', "line 1");
    CHECK(cuvs_status_next_line(t, 4, &pos, &line, &n) == 1 && n == 1 && line[0] == 'y', "line 2");
    CHECK(cuvs_status_next_line(t, 4, &pos, &line, &n) == 0, "end");

    pos = 0;
    CHECK(cuvs_status_next_line("x\ny", 3, &pos, &line, &n) == 1, "first ok");
    CHECK(cuvs_status_next_line("x\ny", 3, &pos, &line, &n) == -1,
          "missing trailing newline -> -1");

    pos = 0;
    CHECK(cuvs_status_next_line("\n", 1, &pos, &line, &n) == 1 && n == 0, "empty line");
}

static void
test_split(void)
{
    char *f[4];
    char b1[] = "1\t\tabc\n";
    char b2[] = "1\t2\n";
    char b3[] = "1\t2\t3\t4\n";
    char b4[] = "\t\t\n";
    char b5[] = "1\t2\0x\n";

    CHECK(cuvs_status_split(b1, 6, f, 3) == 3 && strcmp(f[0], "1") == 0 &&
          strcmp(f[1], "") == 0 && strcmp(f[2], "abc") == 0, "3 fields, empty middle");
    CHECK(cuvs_status_split(b2, 3, f, 3) == -1, "too few fields");
    CHECK(cuvs_status_split(b3, 7, f, 3) == -1, "too many fields");
    CHECK(cuvs_status_split(b4, 2, f, 3) == 3 && f[2][0] == '\0', "all empty fields");
    CHECK(cuvs_status_split(b5, 5, f, 2) == -1, "embedded NUL rejected");
}

static void
test_parse_field(void)
{
    int64_t v;
    int isnull;

#define P(kind, s) cuvs_status_parse_field((kind), (s), &v, &isnull)
    CHECK(P(CUVS_SF_OID, "4294967295") == 0 && v == 4294967295LL && !isnull, "oid max");
    CHECK(P(CUVS_SF_OID, "4294967296") == -1, "oid overflow");
    CHECK(P(CUVS_SF_OID, "-1") == -1, "oid negative");
    CHECK(P(CUVS_SF_OID, "") == -1, "oid empty");
    CHECK(P(CUVS_SF_OID, "12x") == -1, "oid trailing junk");
    CHECK(P(CUVS_SF_OID, " 12") == -1, "oid leading space");
    CHECK(P(CUVS_SF_OID, "+12") == -1, "oid plus sign");

    CHECK(P(CUVS_SF_GPU, "-1") == 0 && isnull, "gpu -1 -> NULL");
    CHECK(P(CUVS_SF_GPU, "0") == 0 && !isnull && v == 0, "gpu 0");
    CHECK(P(CUVS_SF_GPU, "3") == 0 && !isnull && v == 3, "gpu 3");
    CHECK(P(CUVS_SF_INT, "-1") == 0 && !isnull && v == -1, "int -1 stays");
    CHECK(P(CUVS_SF_INT, "2147483648") == -1, "int overflow");

    CHECK(P(CUVS_SF_I64, "9223372036854775807") == 0 && v == INT64_MAX, "i64 max");
    CHECK(P(CUVS_SF_I64, "9223372036854775808") == -1, "i64 overflow");
    CHECK(P(CUVS_SF_I64, "0") == 0 && !isnull, "i64 zero not NULL");

    CHECK(P(CUVS_SF_TIME, "0") == 0 && isnull, "time 0 -> NULL");
    CHECK(P(CUVS_SF_TIME, "1762142400") == 0 && !isnull && v == 1762142400, "time epoch");
    CHECK(P(CUVS_SF_TIME, "-5") == -1, "time negative");

    CHECK(P(CUVS_SF_BOOL, "1") == 0 && v == 1 && !isnull, "bool 1");
    CHECK(P(CUVS_SF_BOOL, "0") == 0 && v == 0 && !isnull, "bool 0");
    CHECK(P(CUVS_SF_BOOL, "2") == -1, "bool 2");
    CHECK(P(CUVS_SF_BOOL, "-1") == -1, "bool -1");

    CHECK(P(CUVS_SF_TRI, "-1") == 0 && isnull, "tri -1 -> NULL");
    CHECK(P(CUVS_SF_TRI, "1") == 0 && v == 1 && !isnull, "tri 1");
    CHECK(P(CUVS_SF_TRI, "0") == 0 && v == 0 && !isnull, "tri 0");
    CHECK(P(CUVS_SF_TRI, "2") == -1, "tri 2");

    CHECK(P(CUVS_SF_TEXT, "") == 0 && isnull, "text empty -> NULL");
    CHECK(P(CUVS_SF_TEXT, "resident") == 0 && !isnull, "text value");
    CHECK(P('?', "1") == -1, "unknown kind");
#undef P
}

/* A full INDEX_STATUS reply parsed the way pg_cuvs_index_status walks it. */
static void
test_index_reply(void)
{
    char text[] =
        CUVS_INDEX_STATUS_COLS "\n"
        "16384\t24576\tresident\tcagra\t0\t1048576\t1762142400\t1762142300\t"
        "7\t1\t0\t0\t1\t\t1\t2\tdone\t3\t0\t\t-1\t0\n"
        "16384\t24577\tunloaded\tbf\t-1\t0\t0\t0\t"
        "0\t0\t0\t0\t0\tsidecar\t0\t0\t\t0\t0\treclaim\t1\t1\n";
    size_t len = strlen(text), pos = 0, n;
    const char *line;
    char *f[CUVS_INDEX_STATUS_NCOLS];
    const char *kinds = CUVS_INDEX_STATUS_KINDS;
    int rows = 0, bad = 0;

    CHECK(cuvs_status_check_header(text, len, CUVS_INDEX_STATUS_COLS, &pos) == 1, "header");
    while (cuvs_status_next_line(text, len, &pos, &line, &n) == 1)
    {
        if (cuvs_status_split((char *) line, n, f, CUVS_INDEX_STATUS_NCOLS) != CUVS_INDEX_STATUS_NCOLS)
        { bad++; continue; }
        for (int c = 0; c < CUVS_INDEX_STATUS_NCOLS; c++)
        {
            int64_t v; int isnull;
            if (cuvs_status_parse_field(kinds[c], f[c], &v, &isnull) != 0)
            { bad++; printf("  col %d '%s' kind %c\n", c, f[c], kinds[c]); }
            if (rows == 1 && c == 4) CHECK(isnull, "row 2 gpu -1 -> NULL");
            if (rows == 0 && c == 20) CHECK(isnull, "row 1 self_replace -1 -> NULL");
            if (rows == 1 && c == 20) CHECK(!isnull && v == 1, "row 2 self_replace 1");
            if (rows == 0 && c == 13) CHECK(isnull, "row 1 empty reclaim_exempt -> NULL");
        }
        rows++;
    }
    CHECK(rows == 2 && bad == 0, "2 rows parse cleanly (rows=%d bad=%d)", rows, bad);
}

static void
test_visibility(void)
{
    /* privileged: everything, including other databases */
    CHECK(cuvs_status_row_visible(1, 5, 7, 0), "priv sees other db");
    CHECK(cuvs_status_row_visible(1, 7, 7, 0), "priv sees unowned");
    /* unprivileged: only own db AND owned */
    CHECK(cuvs_status_row_visible(0, 7, 7, 1), "owner sees own row");
    CHECK(!cuvs_status_row_visible(0, 7, 7, 0), "non-owner hidden");
    CHECK(!cuvs_status_row_visible(0, 5, 7, 1), "other db hidden even if owns");
    CHECK(!cuvs_status_row_visible(0, 5, 7, 0), "other db, not owned hidden");
}

static void
test_msg_in_use(void)
{
    CHECK(cuvs_status_msg_in_use("pinned; unpin first"), "pinned");
    CHECK(cuvs_status_msg_in_use("in use by 2 searches"), "in use");
    CHECK(!cuvs_status_msg_in_use("not loaded"), "not loaded");
    CHECK(!cuvs_status_msg_in_use(""), "empty");
    CHECK(!cuvs_status_msg_in_use("index is pinned"), "pinned not at start");
    CHECK(!cuvs_status_msg_in_use("pin"), "prefix shorter than pinned");
}

static void
test_json_body(void)
{
    size_t off = 0, n = 0;
    const char *a = "{\"reclaim_mode\":\"auto\",\"f\":3}";
    const char *b = "  {  \"x\":1 }\n";

    CHECK(cuvs_status_json_body(a, strlen(a), &off, &n) == 1 && off == 1 &&
          n == strlen(a) - 2 && strncmp(a + off, "\"reclaim_mode\"", 14) == 0, "plain object");
    CHECK(cuvs_status_json_body(b, strlen(b), &off, &n) == 1 && n == 5 &&
          strncmp(b + off, "\"x\":1", 5) == 0, "whitespace trimmed");
    CHECK(cuvs_status_json_body("{}", 2, &off, &n) == 0, "empty object");
    CHECK(cuvs_status_json_body("{ \n }", 5, &off, &n) == 0, "blank object");
    CHECK(cuvs_status_json_body("[1]", 3, &off, &n) == 0, "array");
    CHECK(cuvs_status_json_body("{\"x\":1", 6, &off, &n) == 0, "unterminated");
    CHECK(cuvs_status_json_body("", 0, &off, &n) == 0, "empty");
    CHECK(cuvs_status_json_body("unknown op", 10, &off, &n) == 0, "error text");
    CHECK(cuvs_status_json_body("{\"x\":\0}", 7, &off, &n) == 0, "embedded NUL");
}

int
main(void)
{
    test_layout_constants();
    test_header();
    test_next_line();
    test_split();
    test_parse_field();
    test_index_reply();
    test_visibility();
    test_msg_in_use();
    test_json_body();
    printf("test_status_text: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

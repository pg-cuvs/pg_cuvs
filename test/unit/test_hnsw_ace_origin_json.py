#!/usr/bin/env python3
"""Render the production build_info ACE JSON on stored receipts, and on planted defects.

pg_cuvs_hnsw_build_info() needs a backend, so this compiles the code of the real
function that turns a stored receipt and ACE readback into JSON (from the
`appendStringInfo` of the receipt fields to the ACE readback block) against the
real policy header, with a string buffer in place of StringInfo. Expected values
are written out here. A receipt of a pg_cuvs built with the SDK source planner
reads as before; one built without it says its partition count is the requested
one; a mutated copy of the source must fail this test.
"""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]


def extract(source):
    info = source.index('pg_cuvs_hnsw_build_info(PG_FUNCTION_ARGS)')
    start = source.index('    appendStringInfo(&json,\n        "{\\"status\\":\\"recorded\\"', info)
    end = source.index('    /* Before the NN branch:', start)
    return source[start:end]


PREFIX = r'''
#include "cuvs_hnsw_policy.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef struct { char data[4096]; size_t len; } Buffer;
static void appendStringInfo(Buffer *p, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    int n = vsnprintf(p->data + p->len, sizeof(p->data) - p->len, fmt, a); va_end(a);
    assert(n >= 0 && (size_t)n < sizeof(p->data) - p->len); p->len += (size_t)n;
}
static void appendStringInfoString(Buffer *p, const char *s) { appendStringInfo(p, "%s", s); }
static Buffer render(CuvsHnswPolicy policy, CuvsHnswPolicyReadStatus ace_status, CuvsHnswAceReadback ace) {
    Buffer json = {{0}, 0};
'''
SUFFIX = r'''
    return json;
}
static CuvsHnswPolicy receipt(uint32_t route, uint64_t rows, uint32_t partitions, uint32_t flags) {
    CuvsHnswPolicy p = {0};
    p.magic = CUVS_HNSW_POLICY_MAGIC; p.version = CUVS_HNSW_POLICY_VERSION; p.bytes = sizeof(p);
    p.route = route; p.internal_builder = route; p.guard_passed = 1; p.requested_ef = 64;
    p.requested_algo = route == CUVS_HNSW_ROUTE_ACE ? CUVS_HNSW_ALGO_ACE : CUVS_HNSW_ALGO_IVF_PQ;
    p.degree = 64; p.intermediate = 128; p.ace_ef = 240; p.n_rows = rows; p.dim = 8;
    p.partitions = route == CUVS_HNSW_ROUTE_ACE ? partitions : 0; p.route_flags = flags;
    return p;
}
static Buffer stored(uint32_t route, uint64_t rows, uint32_t partitions, uint32_t flags, int tail) {
    CuvsHnswPolicy p = receipt(route, rows, partitions, flags), out;
    CuvsHnswAceReadback r, got = {0};
    unsigned char page[sizeof(CuvsHnswPolicy) + sizeof(CuvsHnswAceReadback)];
    CuvsHnswPolicyReadStatus ace_status = CUVS_HNSW_POLICY_NOT_RECORDED;
    cuvs_hnsw_ace_readback_fill(&r, CUVS_HNSW_ACE_QUALITY_AUTO, 240, 64, rows, partitions, 20);
    memcpy(page, &p, sizeof(p)); memcpy(page + sizeof(p), &r, sizeof(r));
    assert(cuvs_hnsw_policy_read(page, sizeof(p), &out) == CUVS_HNSW_POLICY_RECORDED);
    if (route == CUVS_HNSW_ROUTE_ACE && tail)
        ace_status = cuvs_hnsw_ace_readback_read(page + sizeof(p), sizeof(r), &out, &got);
    return render(out, ace_status, got);
}
#define HAS(b, s) do { if (!strstr((b).data, (s))) { fprintf(stderr, "missing %s in %s\n", (s), (b).data); return 2; } } while (0)
#define LACKS(b, s) do { if (strstr((b).data, (s))) { fprintf(stderr, "unexpected %s in %s\n", (s), (b).data); return 2; } } while (0)
int main(void) {
    const uint32_t F = CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED;
    /* Receipt of a build with the planner (no mark): as before the mark existed. */
    Buffer b = stored(CUVS_HNSW_ROUTE_ACE, 999000, 2, 0, 1);
    HAS(b, "\"ace_partitions\":2,\"ace_partitions_origin\":\"same_SDK_stable_explicit_plan\"");
    HAS(b, "\"ace_local_nn_descent_iterations\":20");
    b = stored(CUVS_HNSW_ROUTE_ACE, 1000000, 2, 0, 1);
    HAS(b, "\"ace_partitions_origin\":\"same_SDK_stable_explicit_plan\"");
    HAS(b, "\"ace_local_nn_descent_iterations\":20");
    /* Without the planner: the count is the requested one. */
    b = stored(CUVS_HNSW_ROUTE_ACE, 999000, 2, F, 1);
    HAS(b, "\"ace_partitions\":2,\"ace_partitions_origin\":\"requested_effective_not_guaranteed\"");
    LACKS(b, "same_SDK_stable_explicit_plan");
    HAS(b, "\"ace_local_nn_descent_iterations\":20");           /* under 1e6 rows: independent of the count */
    b = stored(CUVS_HNSW_ROUTE_ACE, 1000000, 2, F, 1);
    HAS(b, "\"ace_partitions_origin\":\"requested_effective_not_guaranteed\"");
    HAS(b, "\"ace_local_nn_descent_iterations\":\"not_guaranteed\"");
    LACKS(b, "\"ace_local_nn_descent_iterations\":20");
    b = stored(CUVS_HNSW_ROUTE_ACE, 3000000, 4, F, 1);
    HAS(b, "\"ace_local_nn_descent_iterations\":\"varies_by_partition\"");
    b = stored(CUVS_HNSW_ROUTE_ACE, 1000000, 2, F, 0);          /* an index that has no readback tail */
    HAS(b, "\"ace_partitions_origin\":\"requested_effective_not_guaranteed\"");
    HAS(b, "\"ace_readback\":\"not_recorded\"");
    LACKS(b, "ace_local_nn_descent_iterations");
    /* Off the ACE route there is nothing to say. */
    b = stored(CUVS_HNSW_ROUTE_IVF_PQ, 999000, 0, 0, 0);
    HAS(b, "\"ace_partitions_origin\":\"not_applicable\"");
    HAS(b, "\"ace_readback\":\"not_applicable\"");
    puts("ACE build_info JSON: planner receipt unchanged, requested-only receipt marked, exact iterations withheld from 1e6 rows: PASS (CPU fixture)");
    return 0;
}
'''


def run(source_text, label):
    branch = extract(source_text)
    with tempfile.TemporaryDirectory(prefix='pgcuvs-ace-origin-') as tmp:
        d = Path(tmp)
        (d / 'test.c').write_text(PREFIX + branch + SUFFIX)
        built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'src'),
                                str(d / 'test.c'), '-o', str(d / 'test')], capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(label + ': fixture compile failed: ' + built.stderr)
        ran = subprocess.run([str(d / 'test')], capture_output=True, text=True)
        return ran.returncode, ran.stdout + ran.stderr


MUTANTS = [
    ('fixed_origin', '        cuvs_hnsw_ace_partitions_origin(&policy),\n',
     '        policy.route == CUVS_HNSW_ROUTE_ACE ? "same_SDK_stable_explicit_plan" : "not_applicable",\n'),
    ('iterations_always_exact', '        if (!cuvs_hnsw_ace_iterations_guaranteed(&policy, &ace))',
     '        if (0)'),
]

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path, default=root / 'src/hnsw_export.c')
    p.add_argument('--no-mutants', action='store_true')
    a = p.parse_args()
    text = a.source.read_text()
    code, out = run(text, 'production')
    print(out, end='')
    if code:
        sys.exit(code)
    if not a.no_mutants:
        for name, old, new in MUTANTS:
            assert text.count(old) == 1, ('MUTANT_ANCHOR', name, text.count(old))
            code, out = run(text.replace(old, new, 1), name)
            if code != 2:
                raise AssertionError('MUTANT_SURVIVED:%s (exit %d): %s' % (name, code, out))
            print('mutation %s: detected (%s)' % (name, out.strip().splitlines()[0][:100]))

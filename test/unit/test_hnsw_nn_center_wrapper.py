#!/usr/bin/env python3
"""Execute the production wrapper's graph lambda and serializer block (L2 graph-input centering)
with CPU mocks of cuVS. Checks by running, not by text:
  - cagra::build receives the column-centered copy with the right shape (L2)
    and the original rows otherwise (IP);
  - a centering failure throws before any build;
  - the centered copy is freed when the lambda returns (global new/delete);
  - serialize_to_hnswlib / hnsw::from_cagra receive the ORIGINAL data pointer
    and the original bytes are unchanged (stored vectors stay original at the wrapper boundary).
Mutants are applied to the extracted wrapper text; a mutant whose anchor is
missing is an error, not a kill."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
wrapper = (root / 'src/cuvs_wrapper.cu').read_text()


def block(start_marker, end_marker):
    a = wrapper.index(start_marker)
    b = wrapper.index(end_marker, a) + len(end_marker)
    return wrapper[a:b]


graph_src = block('        auto graph = [&]() {', '\n        }();\n')
serial_src = block('        if (!params->hierarchy && !disk)\n',
                   '                hnsw::serialize(res, output, *index);\n        }\n')
# precision step of the prepare phase (decides whether an L2 offset input is refused)
precision_src = block('        if (prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT)\n        {\n            /* L2 graph input is centered',
                      '                return CUVS_STATUS_BUILD_FAILED;\n            }\n        }\n')

prefix = r'''
#include "cuvs_hnsw_build_common.h"
#include "cuvs_hnsw_nn_center.h"
#include "cuvs_wrapper.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

/* allocation tracking: which blocks are still live */
static std::set<void *> *live = nullptr;
static bool tracking = false;
void *operator new(std::size_t n) {
    void *p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    if (tracking) { tracking = false; live->insert(p); tracking = true; }
    return p;
}
void operator delete(void *p) noexcept {
    if (tracking && p) { tracking = false; live->erase(p); tracking = true; }
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept { operator delete(p); }

struct View {
    const float *p; int64_t r, c;
    const float *data_handle() const { return p; }
    int64_t extent(int i) const { return i ? c : r; }
};
struct Res { void sync_stream() {} };
struct HostMatrix { std::vector<uint32_t> v; int64_t cols;
    uint32_t &operator()(int64_t r, int64_t c) { return v[r * cols + c]; }
    HostMatrix &view() { return *this; } };
namespace raft {
template <class T, class I> View make_host_matrix_view(const float *p, I r, I c) { return View{p, (int64_t)r, (int64_t)c}; }
template <class T, class I> HostMatrix make_host_matrix(I r, I c) { return HostMatrix{std::vector<uint32_t>((size_t)(r * c)), (int64_t)c}; }
inline HostMatrix &make_const_mdspan(HostMatrix &m) { return m; }
}
struct Seen { const float *ptr = nullptr; int64_t rows = 0, cols = 0; std::vector<float> values; int calls = 0; };
static Seen built, serialized, hierarchy;
static void capture(Seen &s, const View &v) {
    s.ptr = v.p; s.rows = v.r; s.cols = v.c; s.values.assign(v.p, v.p + v.r * v.c); s.calls++;
}
namespace cagra {
template <class T, class I> struct index { index(Res &, int) {} index() {} void update_graph(Res &, HostMatrix &) {} };
template <class CP> index<float, uint32_t> build(Res &, const CP &, const View &v) { capture(built, v); return {}; }
inline void serialize_to_hnswlib(Res &, const std::string &, const index<float, uint32_t> &, const View &v) { capture(serialized, v); }
}
struct HnswIndex { std::string file_path() const { return "disk.hnsw"; } };
namespace hnsw {
inline std::unique_ptr<HnswIndex> from_cagra(Res &, int, const cagra::index<float, uint32_t> &, const View &v) {
    capture(hierarchy, v); return std::make_unique<HnswIndex>(); }
inline void serialize(Res &, const std::string &, const HnswIndex &) {}
}
CuvsBfIndex cuvs_bf_build(const float *, int64_t, int, uint32_t, uint32_t, int) { return nullptr; }
void cuvs_bf_free(CuvsBfIndex, int) {}
int cuvs_bf_search_batch(CuvsBfIndex, const float *, int, int, int, CuvsSearchResult *, int) { return 1; }

struct Params { uint64_t n_vecs; uint32_t dim, metric, hierarchy; };
struct Route { uint32_t route; };
struct Prepared { Route route; CuvsHnswNnPrecision nn_precision; };
struct Files { const void *mapping; };

/* the wrapper's prepare-phase precision step, verbatim */
static int run_precision(const Params *params, const float *rows, char *error, size_t error_len) {
    Prepared prepared{{CUVS_HNSW_ROUTE_NN_DESCENT}, {}};
    Files files{rows};
@@PRECISION@@
    return 0;
}

static int run_build(const Params *params, const float *rows, bool tiny, bool disk) {
    Res res; int hp = 0; uint32_t degree = 4; int device_id = 0; int owner_fd = -1;
    struct CP { int metric; } cp{(int)params->metric};
    Prepared prepared{{CUVS_HNSW_ROUTE_NN_DESCENT}, {}};
    const char *workdir = "cpu-fixture";
    View data{rows, (int64_t)params->n_vecs, (int64_t)params->dim};
    (void)degree; (void)device_id; (void)hp; (void)owner_fd;
'''
middle = r'''
    std::string output = "result.hnsw";
    tracking = false;
'''
suffix = r'''
    return 0;
}

int main() {
    live = new std::set<void *>();
    const uint64_t n = 6; const uint32_t d = 4;           /* rows != dim on purpose */
    std::vector<float> rows(n * d);
    for (uint64_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < d; j++) rows[i * d + j] = 1000.0f + (float)(i * 10 + j);
    const std::vector<float> original = rows;

    for (int hier = 0; hier <= 1; hier++) {
        built = serialized = hierarchy = Seen();
        Params p{n, d, CUVS_METRIC_L2, (uint32_t)hier};
        tracking = true;
        run_build(&p, rows.data(), false, false);
        /* graph input: centered copy, right shape, freed by the time serialize ran */
        assert(built.calls == 1 && built.ptr != rows.data() && built.rows == (int64_t)n && built.cols == (int64_t)d);
        for (uint32_t j = 0; j < d; j++) {
            double s = 0; for (uint64_t i = 0; i < n; i++) s += built.values[i * d + j];
            assert(std::fabs(s) < 1e-3);
        }
        for (uint64_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < d; j++)
                assert(std::fabs(built.values[i * d + j] - (rows[i * d + j] - (1000.0f + 25.0f + (float)j))) < 1e-3);
        assert(live->count((void *)built.ptr) == 0);
        /* serializers / upper levels get the original rows, unchanged */
        Seen &out = hier ? hierarchy : serialized;
        assert(out.calls == 1 && out.ptr == rows.data() && out.values == original);
        assert((hier ? serialized.calls : hierarchy.calls) == 0);
        assert(rows == original);
    }
    {   /* IP: no centering */
        built = serialized = hierarchy = Seen();
        Params p{n, d, CUVS_METRIC_IP, 0};
        run_build(&p, rows.data(), false, false);
        assert(built.ptr == rows.data() && built.values == original);
    }
    {   /* prepare-phase precision: an L2 offset input is built (centered), not
         * refused; dtype comes from the original rows (FP32 here) */
        char error[256] = {};
        std::vector<float> tight(n * d);   /* offset 1000, spread 0.01-scale: ratio >> 4096 */
        for (uint64_t i = 0; i < n * d; i++) tight[i] = 1000.0f + 0.01f * (float)i;
        CuvsHnswNnPrecision raw;
        assert(cuvs_hnsw_nn_precision(tight.data(), n, d, CUVS_METRIC_L2, &raw) == CUVS_HNSW_NN_PRECISION_CANCELLATION);
        Params p{n, d, CUVS_METRIC_L2, 0};
        assert(run_precision(&p, tight.data(), error, sizeof(error)) == 0);
        Params ip{n, d, CUVS_METRIC_IP, 0};
        assert(run_precision(&ip, tight.data(), error, sizeof(error)) == 0);
        std::vector<float> nan = rows; nan[3] = NAN;
        assert(run_precision(&p, nan.data(), error, sizeof(error)) == CUVS_STATUS_BUILD_FAILED);
    }
    {   /* centering failure throws before cagra::build */
        built = Seen();
        std::vector<float> bad = rows; bad[5] = INFINITY;
        Params p{n, d, CUVS_METRIC_L2, 0};
        bool threw = false;
        try { run_build(&p, bad.data(), false, false); } catch (const std::runtime_error &) { threw = true; }
        assert(threw && built.calls == 0);
    }
    puts("wrapper graph lambda + serializers (L2 centering): PASS (CPU mock)");
    return 0;
}
'''


def fixture(graph, serial, precision):
    return prefix.replace('@@PRECISION@@', precision) + graph + middle + serial + suffix


mutations = {
    'center rows/dim swapped': ('graph', 'cuvs_hnsw_nn_center(data.data_handle(), params->n_vecs, params->dim,',
                                'cuvs_hnsw_nn_center(data.data_handle(), params->dim, params->n_vecs,'),
    'center failure ignored': ('graph', '                    throw std::runtime_error("NN-descent input centering failed");',
                               '                    (void)0;'),
    'centered copy kept static': ('graph', '                std::vector<float> centered(', '                static std::vector<float> centered('),
    'graph built from original': ('graph', 'return cagra::build(res, cp, centered_view);', 'return cagra::build(res, cp, data);'),
    'serializer gets centered copy': ('serial', 'cagra::serialize_to_hnswlib(res, output, graph, data);',
                                      'cagra::serialize_to_hnswlib(res, output, graph, raft::make_host_matrix_view<const float, int64_t>(built.values.data(), (int64_t)params->n_vecs, (int64_t)params->dim));'),
    'IP centered too': ('graph', '                && cuvs_hnsw_nn_centers(params->metric))', '                && 1)'),
    'precision guard on for L2': ('precision', 'params->metric, cuvs_hnsw_nn_centers(params->metric),\n                    &prepared.nn_precision', 'params->metric, 0,\n                    &prepared.nn_precision'),
}


def build_and_run(d, name, g, s, pr):
    (d / 'fixture.cpp').write_text(fixture(g, s, pr))
    exe = d / 'fixture'
    b = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-variable',
                        '-Wno-unused-but-set-variable', '-I', str(root / 'src'), str(d / 'fixture.cpp'),
                        '-lpthread', '-lm', '-o', str(exe)], capture_output=True, text=True)
    if b.returncode:
        raise RuntimeError(f'{name}: COMPILE FAILED (not a kill): {b.stderr[-1500:]}')
    return subprocess.run([str(exe)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='pgcuvs-nn-center-wrapper-') as tmp:
    d = Path(tmp)
    # 1. the production code as it is: must pass
    r = build_and_run(d, 'baseline', graph_src, serial_src, precision_src)
    if r.returncode:
        raise SystemExit(f'BASELINE FAILED (production wrapper behavior): {r.stderr.strip()}')
    print(r.stdout.strip())
    # 2. own mutants: must fail at run time; a missing anchor is an error, not a kill
    parts = {'graph': graph_src, 'serial': serial_src, 'precision': precision_src}
    for name, (part, old, new) in mutations.items():
        if parts[part].count(old) != 1:
            raise SystemExit(f'ANCHOR MISSING (not a kill): {name}')
        p = dict(parts); p[part] = p[part].replace(old, new)
        r = build_and_run(d, name, p['graph'], p['serial'], p['precision'])
        if r.returncode == 0:
            raise SystemExit(f'MUTANT SURVIVED: {name}')
        print(f'{name}: detected at run time (rc={r.returncode})')

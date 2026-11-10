"""Resident IVF-PQ kNN precision and kNN-graph fallback (SIFT1M): mutation checks.

Header mutants (cuvs_hnsw_build_common.h, cuvs_hnsw_policy.h, cuvs_route_preflight.h)
must turn test_hnsw_policy or test_hnsw_auto_overflow RED. The CUDA wrapper, the daemon
and the backend cannot run here, so their wiring is checked at source level, and wiring
mutants applied to the source text must fail those checks.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
CC = os.environ.get("CC", "cc")
TESTS = ["test/unit/test_hnsw_policy.c", "test/unit/test_hnsw_auto_overflow.c"]

HEADER_MUTANTS = {
    # precision rule
    "l2_bound_factor_dropped": ("cuvs_hnsw_build_common.h",
        "metric == CUVS_METRIC_L2 ? 4.0 * max_sq_norm : max_sq_norm", "max_sq_norm"),
    "bound_inclusive": ("cuvs_hnsw_build_common.h",
        "return !(bound < CUVS_HNSW_FP16_MAX);", "return !(bound <= CUVS_HNSW_FP16_MAX);"),
    "metric_ignored": ("cuvs_hnsw_build_common.h",
        "metric == CUVS_METRIC_L2 ? 4.0 * max_sq_norm : max_sq_norm", "4.0 * max_sq_norm"),
    "nonfinite_row_ignored": ("cuvs_hnsw_build_common.h",
        "        if (!isfinite(sum))\n            return INFINITY;\n", ""),
    "nan_bound_kept_fp16": ("cuvs_hnsw_build_common.h",
        "return !(bound < CUVS_HNSW_FP16_MAX);", "return bound >= CUVS_HNSW_FP16_MAX;"),
    # fallback decision
    "fallback_for_explicit_ivf_pq": ("cuvs_hnsw_build_common.h",
        "return requested_algo == CUVS_HNSW_ALGO_AUTO && route == CUVS_HNSW_ROUTE_IVF_PQ",
        "return (requested_algo == CUVS_HNSW_ALGO_AUTO || requested_algo == CUVS_HNSW_ALGO_IVF_PQ) && route == CUVS_HNSW_ROUTE_IVF_PQ"),
    "fallback_ignores_overflow_mode": ("cuvs_hnsw_build_common.h",
        "        && overflow == CUVS_HNSW_OVERFLOW_ACE && n >= 256\n", "        && n >= 256\n"),
    "fallback_ignores_route": ("cuvs_hnsw_build_common.h",
        "requested_algo == CUVS_HNSW_ALGO_AUTO && route == CUVS_HNSW_ROUTE_IVF_PQ\n",
        "requested_algo == CUVS_HNSW_ALGO_AUTO\n"),
    "fallback_ignores_ace_minimum": ("cuvs_hnsw_build_common.h",
        "overflow == CUVS_HNSW_OVERFLOW_ACE && n >= 256", "overflow == CUVS_HNSW_OVERFLOW_ACE"),
    "fallback_on_any_error": ("cuvs_hnsw_build_common.h",
        "        && what && strstr(what, CUVS_HNSW_GRAPH_KNN_REJECTED) != NULL;", "        ;"),
    # receipt validation
    "fallback_receipt_not_exclusive": ("cuvs_hnsw_policy.h",
        "            + !!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK)\n", ""),
    "fp32_flag_any_route": ("cuvs_hnsw_policy.h",
        "        && (!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_IVFPQ_FP32)\n            || p->route == CUVS_HNSW_ROUTE_IVF_PQ)\n", ""),
    "new_flags_unknown": ("cuvs_hnsw_policy.h",
        "#define CUVS_HNSW_ROUTE_FLAGS_KNOWN 0x3fu", "#define CUVS_HNSW_ROUTE_FLAGS_KNOWN 0xfu"),
    # NOTICE
    "fallback_notice_dropped": ("cuvs_route_preflight.h",
        "        return CUVS_HNSW_NOTICE_GRAPH_FALLBACK;\n", "        return 0;\n"),
}

WRAPPER = SRC / "cuvs_wrapper.cu"
SERVER = SRC / "pg_cuvs_server.c"
BACKEND = SRC / "pg_cuvs.c"
EXPORT = SRC / "hnsw_export.c"

WIRING = {
    "norm_uses_rmm_policy": (WRAPPER, r"return thrust::transform_reduce\(raft::resource::get_thrust_policy\(res\),"),
    "norm_policy_header": (WRAPPER, r"#include <raft/core/resource/thrust_policy.hpp>"),
    "precision_resident_only": (WRAPPER, r"if \(!tiny && !disk\)\s*\{\s*const double max_sq = cuvs_hnsw_max_sq_norm\("
                                         r"static_cast<const float \*>\(files\.mapping\),\s*params->n_vecs, params->dim\);"),
    "precision_three_dtypes": (WRAPPER, r"if \(fp32\)\s*\{\s*auto &sp = prepared\.pq_params->search_params;\s*"
                                        r"sp\.lut_dtype = CUDA_R_32F;\s*sp\.internal_distance_dtype = CUDA_R_32F;\s*"
                                        r"sp\.coarse_search_dtype = CUDA_R_32F;\s*receipt\.route_flags \|= CUVS_HNSW_ROUTE_FLAG_IVFPQ_FP32;"),
    "precision_rule_used": (WRAPPER, r"const bool fp32 = cuvs_hnsw_ivfpq_needs_fp32\(max_sq, params->metric\);"),
    "fallback_decision": (WRAPPER, r"catch \(const std::exception &e\)\s*\{[^{}]*if \(!cuvs_hnsw_graph_failure_to_ace\(params->build_algo, "
                                   r"prepared\.route\.route,\s*params->auto_overflow, params->n_vecs, e\.what\(\)\)\)\s*throw;"),
    "fallback_resolves_ace": (WRAPPER, r"cuvs_hnsw_resolve_route\(params->n_vecs, params->dim, degree, intermediate,\s*"
                                       r"CUVS_HNSW_ALGO_ACE, params->auto_overflow"),
    "fallback_receipt": (WRAPPER, r"receipt\.route = CUVS_HNSW_ROUTE_ACE;[\s\S]{0,700}"
                                  r"CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK;\s*return execute_hnsw_file_build\("),
    "server_allows_recorded_fallback": (SERVER, r"int graph_fallback = \(wire\.policy\.route_flags & CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK\)\s*"
                                                r"&& route\.decision\.route == CUVS_HNSW_ROUTE_IVF_PQ && !route\.decision\.overflowed\s*"
                                                r"&& wire\.policy\.route == CUVS_HNSW_ROUTE_ACE;\s*if \(reply\.status == CUVS_STATUS_OK && !graph_fallback"),
    "backend_notice": (BACKEND, r"if \(notices & CUVS_HNSW_NOTICE_GRAPH_FALLBACK\)\s*ereport\(NOTICE"),
    "receipt_json_fields": (EXPORT, r"\\\"ivfpq_distance_dtype\\\":\\\"%s\\\",\\\"graph_fallback\\\":%s"),
    # CAGRA AM: one helper (cagra_knn_precision) for the build (cuvs_cagra_build_multi)
    # and the compaction rebuild (cuvs_cagra_compact -> cagra::merge -> cagra::build)
    "cagra_bound_on_device": (WRAPPER, r"const double max_sq = may_use_ivf_pq\s*\?\s*device_max_sq_norm\(res, rows_data, rows, dim, ld\)"),
    "cagra_bound_uses_stride": (WRAPPER, r"const float \*r = data \+ row \* ld;"),
    "cagra_build_calls_helper": (WRAPPER, r"cagra_knn_precision\(res, d_corpus\.data_handle\(\), total, \(int64_t\)dim, \(int64_t\)dim,\s*"
                                          r"d_corpus\.extents\(\), metric, build_algo, params, \"cuvs_cagra_build_multi\"\);\s*"
                                          r"auto idx = cuvs::neighbors::cagra::build\(\s*res,\s*params,"),
    "cagra_compact_calls_helper": (WRAPPER, r"cagra_knn_precision\(res, ds\.data_handle\(\), ds\.extent\(0\), ds\.extent\(1\), ds\.stride\(0\),\s*"
                                            r"raft::make_extents<int64_t>\(n_keep, ds\.extent\(1\)\), metric, CUVS_CAGRA_BUILD_AUTO,\s*"
                                            r"params, \"cuvs_cagra_compact\"\);\s*"
                                            r"std::vector<cuvs::neighbors::cagra::index<float, uint32_t>\*> indices = \{&impl->idx\};\s*"
                                            r"auto new_idx = cuvs::neighbors::cagra::merge\(res, params, indices, filter\);"),
    "cagra_compact_kept_count": (WRAPPER, r"if \(w == n_words - 1 && \(n_total & 31\)\) bits &= \(1u << \(n_total & 31\)\) - 1u;\s*"
                                          r"n_keep \+= __builtin_popcount\(bits\);"),
    "cagra_rule_used": (WRAPPER, r"const bool fp32 = may_use_ivf_pq && cuvs_hnsw_ivfpq_needs_fp32\(max_sq, metric\);"),
    "cagra_fp32_three_dtypes": (WRAPPER, r"auto fp32_ivf_pq = \[&\]\(gbp::ivf_pq_params pq\) \{\s*pq\.search_params\.lut_dtype = CUDA_R_32F;\s*"
                                         r"pq\.search_params\.internal_distance_dtype = CUDA_R_32F;\s*pq\.search_params\.coarse_search_dtype = CUDA_R_32F;"),
    "cagra_explicit_ivf_pq": (WRAPPER, r"auto pq = gbp::ivf_pq_params\(shape\);\s*params\.graph_build_params = fp32 \? fp32_ivf_pq\(pq\) : pq;"),
    "cagra_auto_pins_choice": (WRAPPER, r"else if \(fp32\)\s*\{\s*if \(cuvs::neighbors::nn_descent::has_enough_device_memory\(res, shape, sizeof\(uint32_t\)\)\)[\s\S]{0,600}"
                                        r"params\.graph_build_params = fp32_ivf_pq\(gbp::ivf_pq_params\(shape, params\.metric\)\);"),
}

WIRING_MUTANTS = {
    "norm_uses_default_cuda_allocator": (WRAPPER, "thrust::transform_reduce(raft::resource::get_thrust_policy(res),",
                                         "thrust::transform_reduce(thrust::cuda::par.on(res.get_stream()),"),
    "norm_policy_header_removed": (WRAPPER, "#include <raft/core/resource/thrust_policy.hpp>\n", ""),
    "coarse_dtype_kept_fp16": (WRAPPER, "                sp.coarse_search_dtype = CUDA_R_32F;\n", ""),
    "precision_on_ace_route": (WRAPPER, "        if (!tiny && !disk)\n        {\n            const double max_sq",
                               "        if (!tiny)\n        {\n            const double max_sq"),
    "fallback_not_caught": (WRAPPER, "            if (!cuvs_hnsw_graph_failure_to_ace(params->build_algo, prepared.route.route,",
                            "            if (true || !cuvs_hnsw_graph_failure_to_ace(params->build_algo, prepared.route.route,"),
    "fallback_flag_not_recorded": (WRAPPER, "                | CUVS_HNSW_ROUTE_FLAG_GRAPH_FALLBACK;\n", ";\n"),
    "server_rejects_fallback": (SERVER, "    if (reply.status == CUVS_STATUS_OK && !graph_fallback\n",
                                "    if (reply.status == CUVS_STATUS_OK\n"),
    "backend_notice_dropped": (BACKEND, "            if (notices & CUVS_HNSW_NOTICE_GRAPH_FALLBACK)\n", "            if (0)\n"),
    "cagra_explicit_keeps_fp16": (WRAPPER, "        params.graph_build_params = fp32 ? fp32_ivf_pq(pq) : pq;\n",
                                  "        params.graph_build_params = pq;\n"),
    "cagra_auto_fp16_ivf_pq": (WRAPPER, "            params.graph_build_params = fp32_ivf_pq(gbp::ivf_pq_params(shape, params.metric));\n",
                               "            params.graph_build_params = gbp::ivf_pq_params(shape, params.metric);\n"),
    "cagra_coarse_kept_fp16": (WRAPPER, "        pq.search_params.coarse_search_dtype = CUDA_R_32F;\n", ""),
    "cagra_bound_skipped": (WRAPPER, "    const bool fp32 = may_use_ivf_pq && cuvs_hnsw_ivfpq_needs_fp32(max_sq, metric);\n",
                            "    const bool fp32 = false;\n"),
    "cagra_bound_ignores_stride": (WRAPPER, "        const float *r = data + row * ld;\n", "        const float *r = data + row * dim;\n"),
    "build_multi_helper_dropped": (WRAPPER, "        cagra_knn_precision(res, d_corpus.data_handle(), total, (int64_t)dim, (int64_t)dim,\n"
                                            "            d_corpus.extents(), metric, build_algo, params, \"cuvs_cagra_build_multi\");\n", ""),
    "compact_helper_dropped": (WRAPPER, "        cagra_knn_precision(res, ds.data_handle(), ds.extent(0), ds.extent(1), ds.stride(0),\n"
                                        "            raft::make_extents<int64_t>(n_keep, ds.extent(1)), metric, CUVS_CAGRA_BUILD_AUTO,\n"
                                        "            params, \"cuvs_cagra_compact\");\n", ""),
    "compact_merge_default_params": (WRAPPER, "cagra::merge(res, params, indices, filter);",
                                     "cagra::merge(res, cuvs::neighbors::cagra::index_params{}, indices, filter);"),
    "compact_shape_all_rows": (WRAPPER, "raft::make_extents<int64_t>(n_keep, ds.extent(1)), metric, CUVS_CAGRA_BUILD_AUTO,",
                               "raft::make_extents<int64_t>(ds.extent(0), ds.extent(1)), metric, CUVS_CAGRA_BUILD_AUTO,"),
    "compact_helper_explicit_ivf_pq": (WRAPPER, "metric, CUVS_CAGRA_BUILD_AUTO,\n            params, \"cuvs_cagra_compact\");",
                                       "metric, CUVS_CAGRA_BUILD_IVF_PQ,\n            params, \"cuvs_cagra_compact\");"),
    "auto_safe_data_pinned": (WRAPPER, "    else if (fp32)\n    {\n        if (cuvs::neighbors::nn_descent::has_enough_device_memory",
                              "    else\n    {\n        if (cuvs::neighbors::nn_descent::has_enough_device_memory"),
    "compact_keep_count_unmasked": (WRAPPER, "            if (w == n_words - 1 && (n_total & 31)) bits &= (1u << (n_total & 31)) - 1u;\n", ""),
}


def run_tests(src_dir: pathlib.Path, work: pathlib.Path) -> int:
    worst = 0
    for t in TESTS:
        exe = work / pathlib.Path(t).stem
        b = subprocess.run([CC, "-std=gnu11", "-Wall", "-Wextra", "-I", str(src_dir), "-o", str(exe),
                            str(ROOT / t), "-lpthread", "-lm"], capture_output=True, text=True)
        if b.returncode != 0:
            return 2
        r = subprocess.run([str(exe)], capture_output=True, text=True).returncode
        if r != 0:
            worst = r if r > 0 else 1
    return worst


def wiring_ok(texts: dict) -> dict:
    return {name: re.search(pat, texts[path]) is not None for name, (path, pat) in WIRING.items()}


def main() -> int:
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        work = pathlib.Path(d)
        msrc = work / "src"
        shutil.copytree(SRC, msrc)
        if run_tests(msrc, work) != 0:
            print("BASELINE FAIL: unmutated tests do not pass")
            return 1
        for name, (fname, old, new) in HEADER_MUTANTS.items():
            text = (SRC / fname).read_text()
            if text.count(old) != 1:
                print(f"ANCHOR {name} ({text.count(old)})"); fails += 1; continue
            (msrc / fname).write_text(text.replace(old, new))
            rc = run_tests(msrc, work)
            (msrc / fname).write_text(text)
            ok = rc not in (0, 2)
            print(f"{'RED ' if ok else 'SURVIVED'} {name} (rc={rc})")
            fails += 0 if ok else 1
    texts = {p: p.read_text() for p in (WRAPPER, SERVER, BACKEND, EXPORT)}
    base = wiring_ok(texts)
    for name, ok in base.items():
        print(f"{'WIRED' if ok else 'MISSING'} {name}")
        fails += 0 if ok else 1
    for name, (path, old, new) in WIRING_MUTANTS.items():
        if texts[path].count(old) != 1:
            print(f"ANCHOR {name} ({texts[path].count(old)})"); fails += 1; continue
        mutated = dict(texts)
        mutated[path] = texts[path].replace(old, new)
        broken = not all(wiring_ok(mutated).values())
        print(f"{'RED ' if broken else 'SURVIVED'} {name}")
        fails += 0 if broken else 1
    total = len(HEADER_MUTANTS) + len(base) + len(WIRING_MUTANTS)
    print(f"hnsw precision mutations: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

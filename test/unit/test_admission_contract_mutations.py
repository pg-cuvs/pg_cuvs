"""Mutations of the real admission estimate header and hook definition; each must turn the unit test RED.

Usage: python3 -I test/unit/test_admission_contract_mutations.py <new-output-dir>
No GPU, daemon or PostgreSQL is needed.
"""
from pathlib import Path
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HEADER = 'cuvs_admission_contract.h'
TEST = 'test/unit/test_admission_contract.c'

MUTATIONS = {
    'ld_alignment_ignored': (
        'ld = 4u * (((uint64_t)dim + 3u) / 4u);', 'ld = (uint64_t)dim;'),
    'scratch_rounds_down': (
        '* scratch_rho_permille + 999u) / 1000u);',
        '* scratch_rho_permille) / 1000u);'),
    'scratch_term_dropped': (
        'needed = cuvs_admission_sat_add(needed, scratch);', ''),
    'corpus_h2d_dropped': (
        'needed = cuvs_admission_sat_add(final_bytes, corpus_h2d);',
        'needed = final_bytes;'),
    'margin_dropped': (
        'return cuvs_admission_sat_add(needed, margin);', 'return needed;'),
    'scratch_scales_h2d': (
        'cuvs_admission_sat_mul(final_bytes / 1000u, scratch_rho_permille),',
        'cuvs_admission_sat_mul((final_bytes + corpus_h2d) / 1000u, scratch_rho_permille),'),
    'mul_wraps': (
        'if (a != 0 && b > CUVS_ADMISSION_BYTES_SATURATED / a)\n'
        '        return CUVS_ADMISSION_BYTES_SATURATED;\n    return a * b;',
        'return a * b;'),
    'add_wraps': (
        'if (a >= CUVS_ADMISSION_BYTES_SATURATED ||\n'
        '        b >= CUVS_ADMISSION_BYTES_SATURATED - a)\n'
        '        return CUVS_ADMISSION_BYTES_SATURATED;\n    return a + b;',
        'return a + b;'),
    'invalid_shape_admits': (
        'if (n < 0 || dim <= 0)\n        return CUVS_ADMISSION_BYTES_SATURATED;',
        'if (n < 0 || dim <= 0)\n        return 0;'),
    'rho_bound_removed': (
        'if (scratch_rho_permille > CUVS_ADMISSION_RHO_PERMILLE_MAX ||\n'
        '        final_bytes >= CUVS_ADMISSION_BYTES_SATURATED)',
        'if (final_bytes >= CUVS_ADMISSION_BYTES_SATURATED)'),
    'zero_degree_not_default': (
        'gd = graph_degree > 0 ?', 'gd = graph_degree >= 0 ?'),
    'hook_emits_ar6_before_i4': (
        '    (void) in;\n    return CUVS_ADMISSION_NONE;',
        '    (void) in;\n    return CUVS_ADMISSION_AR6;'),
}
UTIL_MUTATIONS = {'hook_emits_ar6_before_i4'}

MESSAGE_TEST = 'test/unit/test_admission_message.c'
# DETAIL/HINT builder (cuvs_util.c), checked by the message test.
MESSAGE_MUTATIONS = {
    'other_db_name_looked_up': (
        '    *resolved = 0;\n    if (t->db_oid != cat->my_db_oid)\n',
        '    *resolved = 0;\n    if (0 && t->db_oid != cat->my_db_oid)\n'),
    'hint_ignores_unload_privilege': (
        '        if (cat->can_unload == NULL || !cat->can_unload(cat->ctx, t->db_oid, t->index_oid))\n'
        '            continue;\n',
        ''),
    'idle_never_printed_as_minutes': (
        'if (f->cand[c].idle_s == CUVS_ADMISSION_IDLE_NEVER)',
        'if (f->cand[c].idle_s < CUVS_ADMISSION_IDLE_NEVER)'),
    'zero_candidates_ignores_pins': (
        '    if (f->n_cand == 0)\n    {\n        if (f->k_cnt > 0)',
        '    if (f->n_cand == 0)\n    {\n        if (0)'),
    'wait_line_without_candidates': (
        'if (!load && unload && (f->present & CUVS_ADM_F_R) && f->r == 1)',
        'if (!load && (f->present & CUVS_ADM_F_R) && f->r == 1)'),
    'missing_r_authorizes_wait': (
        'if (!load && unload && (f->present & CUVS_ADM_F_R) && f->r == 1)',
        'if (!load && unload && f->r == 1)'),
    'literal_quote_not_doubled': (
        "cuvs_sb_add(b, *s == '\\'' ? \"''\" : \"%c\", *s);",
        'cuvs_sb_add(b, "%c", *s);'),
    'rebuild_unpin_clause_dropped': (
        '            if (unpin)\n                cuvs_sb_add(&h, CUVS_ADM_UNPIN_CLAUSE);\n', ''),
    'absent_resident_printed_as_zero': (
        '    if (f->present & CUVS_ADM_F_H)\n    {\n        cuvs_sb_add(b, ", resident ");',
        '    if (1)\n    {\n        cuvs_sb_add(b, ", resident ");'),
    'shard_prefix_dropped': (
        '        cuvs_sb_add(&d, "shard %u: ", f->i);', '        ;'),
    'load_ar0_reported_permanent': (
        'return load ? CUVS_ADMISSION_SQLSTATE_MEMORY : state;', 'return state;'),
    'load_hint_says_create_index': (
        'const char *retry = load ? "then retry the query." : "then retry CREATE INDEX.";',
        'const char *retry = "then retry CREATE INDEX.";'),
}

WIRE_TEST = 'test/unit/test_admission_wire.c'
# err_out codec (cuvs_util.c), checked by the wire test under ASAN/UBSan.
WIRE_MUTATIONS = {
    'shape_bracketed_accepts_nn_descent': (
        'if ((ivf || autoa) && gd >= 32 && gd <= 64',
        'if (gd >= 32 && gd <= 64'),
    'shape_checked_ignores_metric': (
        'if (dim != 768 || metric != CUVS_METRIC_L2)', 'if (dim != 768)'),
    'decoder_reads_past_512': (
        'lim = len < CUVS_ADMISSION_ERR_OUT_BYTES ? len : CUVS_ADMISSION_ERR_OUT_BYTES;',
        'lim = len;'),
    'decoder_accepts_fourth_candidate': (
        'if (out->n_cand >= CUVS_ADMISSION_MAX_CANDIDATES ||\n', 'if (\n'),
    'encoder_accepts_fourth_candidate': (
        '    if (in->n_cand > CUVS_ADMISSION_MAX_CANDIDATES)\n        return -1;\n', ''),
    'AR0_reported_as_53200': (
        'if (reason == CUVS_ADMISSION_AR0)\n        return CUVS_ADMISSION_SQLSTATE_LIMIT;',
        'if (reason == CUVS_ADMISSION_AR0)\n        return CUVS_ADMISSION_SQLSTATE_MEMORY;'),
    'permanent_reason_invites_retry': (
        'return state != NULL && strcmp(state, CUVS_ADMISSION_SQLSTATE_MEMORY) == 0;',
        'return state != NULL;'),
    'w_accepts_countdown': (
        'cuvs_admission_int(val, tok_end, CUVS_ADMISSION_WAIT_UNPIN, 0, &si)',
        'cuvs_admission_int(val, tok_end, CUVS_ADMISSION_WAIT_UNPIN, CUVS_ADMISSION_SCALAR_MAX, &si)'),
    'idle_never_rejected': (
        'cuvs_admission_int(s[3], e[3], CUVS_ADMISSION_IDLE_NEVER,',
        'cuvs_admission_int(s[3], e[3], 0,'),
    'mib_not_saturated': (
        'return mib > CUVS_ADMISSION_SCALAR_MAX ? CUVS_ADMISSION_SCALAR_MAX : (uint32_t) mib;',
        'return (uint32_t) mib;'),
    'duplicate_key_accepted': (
        '        if (bit != 0 && (out->present & bit))\n            goto bad;\n', ''),
    'scalar_above_saturation_accepted': (
        '                if (cuvs_admission_uint(val, tok_end, CUVS_ADMISSION_SCALAR_MAX, &a) != 0)\n                    goto bad;\n                *slot',
        '                if (cuvs_admission_uint(val, tok_end, UINT32_MAX, &a) != 0)\n                    goto bad;\n                *slot'),
}


def run(argv):
    q = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=60)
    return q.returncode, q.stdout + q.stderr


IPC_TEST = 'test/unit/test_admission_ipc.c'
# Reply-header I3 fields in the real client (cuvs_ipc.c / cuvs_ipc.h).
IPC_MUTATIONS = {
    'stale_err_out_not_cleared': ('cuvs_ipc.c',
        '    forget_reply();   /* every request starts without a previous reply\'s I3 fields */\n', ''),
    'err_out_not_bounded': ('cuvs_ipc.c',
        "    g_last_err_out[sizeof(g_last_err_out) - 1] = '\\0';\n", ''),
    'reply_fields_not_recorded': ('cuvs_ipc.c',
        '    int rc = recv_all(fd, hdr, sizeof(*hdr));\n    if (rc == 0)\n        remember_reply(hdr);\n',
        '    int rc = recv_all(fd, hdr, sizeof(*hdr));\n'),
    'wire_version_not_bumped': ('cuvs_ipc.h',
        '#define CUVS_PROTO_VERSION 5u', '#define CUVS_PROTO_VERSION 4u'),
}
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
SOURCES = ['cuvs_util.c', 'cuvs_ipc.c', 'cuvs_ipc.h', 'cuvs_build_corpus.c']


def main(output):
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    originals = {n: (ROOT / 'src' / n).read_text() for n in [HEADER] + SOURCES}
    results, ok = [], True
    util = ['cuvs_util.c']
    cases = [(n, o, w, 'cuvs_util.c' if n in UTIL_MUTATIONS else HEADER, TEST, [], util)
             for n, (o, w) in MUTATIONS.items()]
    cases += [(n, o, w, 'cuvs_util.c', WIRE_TEST, SAN, util)
              for n, (o, w) in WIRE_MUTATIONS.items()]
    cases += [(n, o, w, 'cuvs_util.c', MESSAGE_TEST, SAN, util)
              for n, (o, w) in MESSAGE_MUTATIONS.items()]
    cases += [(n, o, w, f, IPC_TEST, SAN + ['-lpthread', '-lm'],
               ['cuvs_ipc.c', 'cuvs_util.c', 'cuvs_build_corpus.c'])
              for n, (f, o, w) in IPC_MUTATIONS.items()]
    for name, old, new, target, test, extra, sources in cases:
        case = out / name
        case.mkdir()
        for fname, body in originals.items():
            if fname == target:
                assert body.count(old) == 1, (name, body.count(old))
                body = body.replace(old, new, 1)
            (case / fname).write_text(body)
        binary = str(case / 'check')
        rc, log = run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-I', str(case), '-I', 'src',
                       '-o', binary, test] + [str(case / f) for f in sources] + extra)
        stage = 'compile'
        if rc == 0:
            stage = 'run'
            rc, log = run([binary])
        red = rc != 0
        ok &= red
        (case / 'log.txt').write_text(log)
        first = next((l for l in log.splitlines() if 'FAIL' in l or 'ERROR' in l or 'error' in l), '')
        results.append(dict(mutation=name, stage=stage, exit=rc, red=red, first=first))
        print(('RED   ' if red else 'GREEN ') + name + ' (' + stage + ') ' + first)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('mutations: %d/%d RED' % (sum(r['red'] for r in results), len(results)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))

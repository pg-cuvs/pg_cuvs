"""#238 source check: refresh_main_bf_cache uses the remembered BF verdict.

The decision itself (cuvs_bf_verdict.h) is unit tested by test_bf_verdict.c.
This checks that the daemon's unsharded brute_force refresh wires it in:

  (a) before opening `.vectors`, it calls
      cuvs_bf_verdict_unavailable(&e->bf_verdict, <identity>, e->n_vecs,
      e->dim, e->metric, tids_ok, cur_tids_crc) with the current `.tids` crc
      from tids_body_crc_of, and returns when that call is true;
  (b) a failed cuvs_vectors_read is noted with its actual return value
      (cuvs_bf_verdict_note(&e->bf_verdict, rc, ...) where
      rc = cuvs_vectors_read(...)), not a constant, so allocation and I/O
      failures stay unremembered; the header mismatches are noted with 0, &vh;
  (c) a successful build clears the verdict (bf_verdict.valid = 0 inside
      if (e->main_bf_idx) after cuvs_bf_build).

The script also replays planted defects (one per rule) against its own rules
and fails if any of them passes.

Usage: check_bf_verdict_source.py [path/to/pg_cuvs_server.c]
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FUNC = 'refresh_main_bf_cache'


def mask(s):
    # Blank comments and string literals, keeping offsets and newlines.
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), s, flags=re.S)


def body(code, name):
    # Text between the braces of the one definition of `name`.
    defs = list(re.finditer(r'^' + name + r'\s*\([^;{]*\)\s*\{', code, flags=re.M))
    if len(defs) != 1:
        return None
    depth, i = 1, defs[0].end()
    while depth and i < len(code):
        depth += {'{': 1, '}': -1}.get(code[i], 0)
        i += 1
    return code[defs[0].end():i - 1]


def call_args(text, start):
    # Arguments of the call whose '(' is at text[start]; returns (args, end).
    depth, i = 1, start + 1
    while depth and i < len(text):
        depth += {'(': 1, ')': -1}.get(text[i], 0)
        i += 1
    parts, depth, cur = [], 0, ''
    for ch in text[start + 1:i - 1]:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(' '.join(cur.split()))
            cur = ''
        else:
            cur += ch
    parts.append(' '.join(cur.split()))
    return parts, i


def calls(text, fn):
    out = []
    for m in re.finditer(r'\b' + fn + r'\s*\(', text):
        args, end = call_args(text, m.end() - 1)
        out.append((m.start(), args, end))
    return out


def check(text):
    code = mask(text)
    b = body(code, FUNC)
    if b is None:
        return [f'expected one definition of {FUNC}()']
    errors = []
    open_at = [m.start() for m in re.finditer(r'\bfopen\s*\(\s*path\s*,', b)]

    # (a) the verdict decides before the file is opened, with current values.
    un = calls(b, 'cuvs_bf_verdict_unavailable')
    if len(un) != 1:
        errors.append(f'(a) expected one cuvs_bf_verdict_unavailable call, found {len(un)}')
    else:
        pos, args, end = un[0]
        want_tail = ['e->n_vecs', 'e->dim', 'e->metric', 'tids_ok', 'cur_tids_crc']
        if not args or args[0] != '&e->bf_verdict':
            errors.append('(a) cuvs_bf_verdict_unavailable must take &e->bf_verdict')
        if len(args) != 9 or args[4:] != want_tail:
            errors.append('(a) cuvs_bf_verdict_unavailable must end with '
                          + ', '.join(want_tail) + f'; got {args[4:]}')
        if not re.search(r'\bif\s*\(\s*$', b[:pos]) or \
           not re.match(r'\s*\)\s*return\s*;', b[end:]):
            errors.append('(a) a true cuvs_bf_verdict_unavailable must return '
                          '(expected "if (cuvs_bf_verdict_unavailable(...)) return;")')
        if not open_at or pos > open_at[0]:
            errors.append('(a) cuvs_bf_verdict_unavailable must come before fopen(path, ...)')
        if not re.search(r'\btids_ok\s*=\s*\(\s*tids_body_crc_of\s*\(\s*e\s*,\s*&\s*cur_tids_crc\s*\)'
                         r'\s*==\s*0\s*\)', b[:pos]):
            errors.append('(a) tids_ok / cur_tids_crc must come from '
                          'tids_body_crc_of(e, &cur_tids_crc) == 0 before the call')

    # (b) a failed read is noted with the read's own return value.
    rd = re.findall(r'\b(\w+)\s*=\s*cuvs_vectors_read\s*\(', b)
    if len(rd) != 1:
        errors.append(f'(b) expected one "<var> = cuvs_vectors_read(...)", found {len(rd)}')
    notes = calls(b, 'cuvs_bf_verdict_note')
    rc_name = rd[0] if len(rd) == 1 else 'rc'
    with_rc = [a for _, a, _ in notes if len(a) >= 3 and a[1] == rc_name]
    if len(with_rc) != 1 or with_rc[0][0] != '&e->bf_verdict':
        errors.append(f'(b) expected one cuvs_bf_verdict_note(&e->bf_verdict, {rc_name}, ...) '
                      f'for the failed read, found {len(with_rc)}')
    for _, a, _ in notes:
        if len(a) < 3 or a[0] != '&e->bf_verdict':
            errors.append(f'(b) cuvs_bf_verdict_note must take &e->bf_verdict: {a}')
        elif a[1] == rc_name:
            continue
        elif a[1] != '0' or a[2] != '&vh':
            errors.append(f'(b) cuvs_bf_verdict_note second argument must be {rc_name} '
                          f'(failed read) or 0 with &vh (header mismatch), got {a[1]}, {a[2]}')

    # (c) a successful build clears the verdict.
    bm = re.search(r'\be->main_bf_idx\s*=\s*cuvs_bf_build\s*\(', b)
    if not bm:
        errors.append('(c) expected e->main_bf_idx = cuvs_bf_build(...)')
    else:
        im = re.search(r'\bif\s*\(\s*e->main_bf_idx\s*\)\s*\{', b[bm.end():])
        blk = None
        if im:
            s = bm.end() + im.end()
            depth, i = 1, s
            while depth and i < len(b):
                depth += {'{': 1, '}': -1}.get(b[i], 0)
                i += 1
            blk = b[s:i - 1]
        if blk is None or not re.search(r'\be->bf_verdict\.valid\s*=\s*0\s*;', blk):
            errors.append('(c) e->bf_verdict.valid = 0 must be set in '
                          'if (e->main_bf_idx) { ... } after cuvs_bf_build')
    return errors


# Planted defects, one per rule: (name, regex, replacement). Each must apply
# to the real source exactly once and make check() fail.
MUTANTS = [
    ('a: no return on a remembered verdict',
     r'(tids_ok, cur_tids_crc\)\)\s*\n\s*)return;', r'\1(void) 0;'),
    ('a: verdict checked against the remembered, not the current, row count',
     r'e->n_vecs, e->dim, e->metric,(\s*\n\s*tids_ok)', r'e->bf_verdict.n_vecs, e->dim, e->metric,\1'),
    ('b: failed read noted as a constant READ_BAD',
     r'cuvs_bf_verdict_note\(&e->bf_verdict, rc,', 'cuvs_bf_verdict_note(&e->bf_verdict, CUVS_VECTORS_READ_BAD,'),
    ('c: successful build keeps the verdict',
     r'\n[ \t]*e->bf_verdict\.valid\s*=\s*0;[^\n]*\n(\s*e->main_bf_n )', r'\n\1'),
]


def replay(text):
    errors = []
    for name, pat, rep in MUTANTS:
        mutant, n = re.subn(pat, rep, text)
        if n != 1:
            errors.append(f'mutant "{name}" applied {n} times (expected 1): update MUTANTS')
        elif not check(mutant):
            errors.append(f'mutant "{name}" passed the check')
    return errors


def main(argv):
    src = pathlib.Path(argv[1]) if len(argv) > 1 else ROOT / 'src' / 'pg_cuvs_server.c'
    text = src.read_text()
    errors = check(text)
    if not errors:
        errors = replay(text)
    for e in errors:
        print(f'FAIL {src.name}: {e}')
    if errors:
        return 1
    print(f'check_bf_verdict_source: ok ({len(MUTANTS)} planted defects caught)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

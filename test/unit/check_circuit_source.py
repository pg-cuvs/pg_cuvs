"""UC-2 source conformance: an open circuit breaker never ends a scan with 0 rows.

Before UC-2 every GPU scan path did
    if (!enable_cuvs || cuvs_circuit_is_open(oid)) return false;
which turned a breaker opened after planning into a silent empty answer. The
scan paths must now go through cuvs_breaker_check_scan(), which raises 53200.
This is a source check, not an execution test: the AM callbacks need a live
backend, so it guards the shape that the unit tests cannot reach.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'src' / 'pg_cuvs.c'

BREAKER_TOKENS = ('cuvs_circuit_is_open', 'cuvs_circuit_state', 'cuvs_breaker_state',
                  'CUVS_CIRCUIT_OPEN', 'CUVS_CIRCUIT_EXEC_ERROR')


def mask(s):
    # Blank comments and string literals, keeping offsets and newlines.
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), s, flags=re.S)


def body(masked, start):
    b = masked.index('{', start)
    depth, i = 1, b + 1
    while depth:
        if masked[i] == '{':
            depth += 1
        elif masked[i] == '}':
            depth -= 1
        i += 1
    return masked[start:i]


def if_conditions(code):
    """Yield (offset, condition, following statement) for each `if (...)`."""
    for m in re.finditer(r'\bif\s*\(', code):
        depth, i = 1, m.end()
        while depth:
            if code[i] == '(':
                depth += 1
            elif code[i] == ')':
                depth -= 1
            i += 1
        cond = code[m.end():i - 1]
        stmt = code[i:code.find(';', i) + 1]
        yield m.start(), cond, stmt


def line_of(s, off):
    return s.count('\n', 0, off) + 1


def check(text):
    errors = []
    code = mask(text)

    # 1. No breaker-condition branch that returns false (the 0-row path).
    for off, cond, stmt in if_conditions(code):
        if any(t in cond for t in BREAKER_TOKENS) and re.fullmatch(
                r'\s*\{?\s*return\s+false\s*;', stmt):
            errors.append(f'line {line_of(text, off)}: breaker-open branch returns false '
                          f'(0-row scan): if ({" ".join(cond.split())}) {stmt.strip()}')

    # 2. Every amgettuple scan path gates through cuvs_breaker_check_scan().
    scans = [m for m in re.finditer(
        r'^static bool\s*\n(\w+_gettuple)\(IndexScanDesc scan, ScanDirection dir\)',
        code, flags=re.M)]
    if len(scans) < 3:
        errors.append(f'expected >= 3 *_gettuple scan paths, found {len(scans)}')
    for m in scans:
        fn = body(code, m.start())
        if 'cuvs_breaker_check_scan(scan->indexRelation)' not in fn:
            errors.append(f'{m.group(1)}: missing cuvs_breaker_check_scan()')
        if 'cuvs_circuit_is_open(' in fn:
            errors.append(f'{m.group(1)}: still reads cuvs_circuit_is_open()')

    # 3. The gate raises 53200 on OPEN, naming the qualified index.
    gate = re.search(r'^cuvs_breaker_check_scan\(Relation index_rel\)', code, flags=re.M)
    if not gate:
        errors.append('cuvs_breaker_check_scan() not defined')
    else:
        # Masking keeps offsets, so the raw slice holds the literal text.
        fn_text = text[gate.start():gate.start() + len(body(code, gate.start()))]
        for needle in ('CUVS_CIRCUIT_EXEC_ERROR', 'ereport(ERROR',
                       'errcode(ERRCODE_OUT_OF_MEMORY)', 'quote_qualified_identifier(',
                       'GPU circuit breaker is open for index', 'errhint(',
                       'pg_cuvs_reset_circuit'):
            if needle not in fn_text:
                errors.append(f'cuvs_breaker_check_scan(): missing {needle}')
    return errors, len(scans)


def main():
    errors, n = check(SRC.read_text())
    if errors:
        for e in errors:
            print(f'[FAIL] {e}')
        return 1
    print(f'PASS circuit source: {n} scan paths gate through cuvs_breaker_check_scan; '
          f'no breaker-open branch returns false')
    return 0


if __name__ == '__main__':
    sys.exit(main())

"""CK-10 runtime log consumer. Missing log families are incomplete, never PASS.

hnsw_route is 13/14 fields under conch decision 8d48e8de (2026-11-03).
No provider, SQL or measurement execution.
"""
import argparse
import json
import re
from pathlib import Path

SCHEMAS = {
    'admit': 'op path gpu db idx algo shape_class auto_probe n dim gd igd final scratch needed limit budget pending_before pending_after quota headroom_raw free_net pool_used logical_used evicted verdict reason res_bytes res_n lru_free elig_free pin_bytes policy overcommit reserved phys_free lw_ub lw_pending host_guard self_res mono_us pool_need headroom pool_quota pending',
    'release': 'op gpu db idx needed pending_after outcome lw_pending_after mono_us',
    'build_oom': 'op gpu db idx needed action mono_us failed_bytes',
    'build_peak': 'op gpu db idx final used_before peak_used used_after reserved_after solo outcome lw_peak spill mono_us',
    'build_result': 'op gpu db idx algo n dim gd igd dataset_attached route build_ms outcome self_replace unloaded mono_us',
    'evict': 'op gpu db idx reason requester_idx idle_s protect_s hits vram_bytes saved_ms mono_us self_replace',
    'reclaim': 'op gpu tick resident_before resident_after hi_bytes lo_bytes evicted skip_sidecar trigger f_bytes f_src mono_us',
    'hnsw_route': 'phase requested_algo route overflow overflowed status rows est_host est_gpu host_budget gpu_budget partitions disk_need',
}
TEXT = set('op path algo shape_class auto_probe verdict reason policy overcommit outcome action route trigger f_src phase requested_algo overflow error'.split())
TAG = re.compile(r'^\[(?:INFO|WARN|ERROR|DEBUG)\] \[(' + '|'.join(SCHEMAS) + r')\] ')


def parse_line(line):
    match = TAG.match(line)
    if not match:
        return None
    tag = match.group(1)
    body = line[match.end():].strip()
    if tag == 'evict' and body.startswith('save_index FAILED for '):
        return 'persist_save_failure', {'diagnostic': body}
    error = None
    if tag == 'hnsw_route' and ' error=' in body:
        body, error = body.split(' error=', 1)
    pairs = [word.split('=', 1) for word in body.split()]
    if any(len(p) != 2 for p in pairs):
        raise ValueError(tag + ': malformed key/value field')
    expected = SCHEMAS[tag].split()
    if [p[0] for p in pairs] != expected:
        raise ValueError(tag + ': field order/count differs from contract')
    row = dict(pairs)
    if error is not None:
        if not error:
            raise ValueError('hnsw_route: empty error field')
        row['error'] = error
    for key, value in row.items():
        if key in TEXT:
            continue
        if key == 'build_ms':
            from decimal import Decimal
            if not Decimal(value).is_finite() or Decimal(value) < 0:
                raise ValueError('build_result: invalid duration')
        elif not re.fullmatch(r'-?\d+', value):
            raise ValueError(tag + ': invalid integer ' + key)
        elif int(value) < 0 and key not in ('gpu', 'idle_s', 'failed_bytes'):
            raise ValueError(tag + ': negative counter ' + key)
    if tag == 'admit':
        n = lambda key: int(row[key])
        if n('needed') != n('final') + n('scratch'):
            raise ValueError('admit: needed != final + scratch')
        if n('free_net') != max(0, n('headroom_raw') - n('pending_after')):
            raise ValueError('admit: physical/pool headroom minus pending differs')
        if (row['verdict'] == 'admit') != (row['reason'] == 'none'):
            raise ValueError('admit: verdict/reason contradict')
        if row['path'] in ('load', 'reload', 'startup', 'warmup'):
            if row['policy'] != 'na' or row['overcommit'] != 'na':
                raise ValueError('load: build-only policy fields populated')
            if row['verdict'] == 'admit' and row['path'] != 'load':
                raise ValueError('automatic reload/startup/warmup admit line is forbidden')
            if n('pending_after') != n('pending_before'):
                raise ValueError('load: reservation changed')
        if row['reason'] == 'AR0' and not n('needed') > n('limit'):
            raise ValueError('admit: AR0 limit does not explain rejection')
        # ADR-089: pool_need = needed + lw_ub; AR8 when the pool quota cannot
        # hold pending + pool_need + headroom. Decided before AR6/AR7 and
        # before eviction.
        ar8 = n('pool_quota') > 0 and n('pending') + n('pool_need') + n('headroom') > n('pool_quota')
        if row['reason'] == 'AR8':
            if n('quota') and n('pending_before') + n('needed') > n('quota'):
                raise ValueError('admit: AR1 takes precedence over AR8')
            if n('pool_need') != n('needed') + n('lw_ub') or not ar8 or n('evicted'):
                raise ValueError('admit: AR8 arithmetic/order differs')
        if row['reason'] in ('AR6', 'AR7'):
            if n('quota') and n('pending_before') + n('needed') > n('quota'):
                raise ValueError('admit: AR1 takes precedence over workspace rejection')
            if ar8:
                raise ValueError('admit: AR8 takes precedence over workspace rejection')
            growth = max(0, n('needed') - max(0, n('reserved') - n('pool_used')))
            spill = max(0, growth + n('lw_ub') + n('lw_pending') - n('phys_free'))
            expected_reason = ('AR6' if spill > 0 else 'none') if row['overcommit'] == 'off' else ('AR7' if spill > n('host_guard') else 'none')
            if row['reason'] != expected_reason or n('evicted'):
                raise ValueError('admit: workspace rejection/order/snapshot differs')
        if row['reason'] == 'AR5':
            shortage = max(max(0, n('logical_used') + n('needed') - n('budget')) if n('budget') else 0,
                           max(0, n('needed') - n('free_net')))
            if n('evicted') or n('lru_free') < shortage:
                raise ValueError('admit: AR5 cannot be solved by scope lru')
    if tag == 'build_oom' and row['action'] != 'fail_no_evict':
        raise ValueError('build_oom: retry/eviction action')
    if tag == 'build_oom' and (int(row['failed_bytes']) == 0 or int(row['failed_bytes']) < -1):
        raise ValueError('build_oom: failed_bytes must be positive or unknown -1')
    if tag == 'build_peak':
        if int(row['peak_used']) < max(int(row['used_before']), int(row['used_after'])):
            raise ValueError('build_peak: peak below live usage')
        if row['solo'] not in ('0', '1') or row['spill'] not in ('0', '1'):
            raise ValueError('build_peak: invalid flags')
    return tag, row


def evaluate(paths):
    counts = dict.fromkeys(SCHEMAS, 0)
    errors = []
    persist_failures = 0
    for path in paths:
        for number, line in enumerate(Path(path).read_text(errors='replace').splitlines(), 1):
            try:
                result = parse_line(line)
                if result:
                    if result[0] == 'persist_save_failure':
                        persist_failures += 1
                    else:
                        counts[result[0]] += 1
            except ValueError as err:
                errors.append({'path': str(path), 'line': number, 'error': str(err)})
    missing = [tag for tag, count in counts.items() if not count]
    return {'status': 'FAIL' if errors else 'INCOMPLETE' if missing else 'PASS',
            'counts': counts, 'missing': missing, 'errors': errors,
            'save_failure_diagnostics': persist_failures,
            'scope': 'schema and encoded arithmetic; runtime coverage listed by family'}

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs', type=Path, nargs='+')
    args = p.parse_args()
    result = evaluate(args.logs)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['status'] == 'PASS' else 1)

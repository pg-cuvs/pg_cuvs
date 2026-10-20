#!/usr/bin/env python3
import argparse
import json
import statistics
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('root', type=Path)
a = p.parse_args()
noise = json.loads((a.root / 'baseline-noise.json').read_text())
labels = ['rmm_zero', 'rmm_zero2', 'rmm_zero3']
raw = {label: json.loads((a.root / label / 'metrics.json').read_text()) for label in labels}
out = {'predeclared_n': 3, 'labels': labels, 'baseline_unchanged': True,
       'criterion': 'recall within frozen span; lower latency outside span is an observation, not a failure',
       'points': {}}
for i, k in enumerate((64, 128)):
    point = {}
    for field in ('recall_at10', 'p50_ms', 'qps'):
        vals = [raw[label][i][field] for label in labels]
        ref = noise['points'][str(k)][field]
        mean = statistics.mean(vals)
        point[field] = {'values': vals, 'mean': mean, 'min': min(vals), 'max': max(vals),
                        'std': statistics.stdev(vals), 'baseline_mean': ref['mean'],
                        'delta_to_baseline_mean': mean - ref['mean'],
                        'within_baseline_span': abs(mean-ref['mean']) <= ref['max']-ref['min']+1e-12}
    point['all_individual_flags'] = [
        {'label': label, **{field: raw[label][i][field] for field in raw[label][i]
                            if field.endswith('_within_control_span')}} for label in labels]
    point['recall_pass'] = point['recall_at10']['within_baseline_span']
    point['latency_pass'] = (point['p50_ms']['within_baseline_span'] or
                             point['p50_ms']['delta_to_baseline_mean'] < 0)
    out['points'][str(k)] = point
(a.root/'rmm-zero-aggregate.json').write_text(json.dumps(out, indent=2)+'\n')
print(json.dumps(out, indent=2))

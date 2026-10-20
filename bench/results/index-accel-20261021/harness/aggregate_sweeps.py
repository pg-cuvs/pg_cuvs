#!/usr/bin/env python3
import argparse
import csv
import importlib.util
import itertools
import json
from pathlib import Path
import statistics
import numpy as np

p = argparse.ArgumentParser()
p.add_argument('root', type=Path)
p.add_argument('--truth', type=Path, required=True)
p.add_argument('--helper', type=Path, required=True)
a = p.parse_args()
spec = importlib.util.spec_from_file_location('helper', a.helper)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
truth = np.load(a.truth, mmap_mode='r')[:10000, :10]
labels = ['issue144_run1', 'issue144_run2', 'issue144_run3']
baseline = json.loads((a.root/'baseline-noise.json').read_text())
meta = [json.loads((a.root/label/'metadata.json').read_text()) for label in labels]
assert all(m['graph'] == baseline['graph'] and
           m['query_bytes_sha256'] == baseline['query_bytes_sha256'] and
           m['truth_bytes_sha256'] == baseline['truth_bytes_sha256'] and
           m['commit'] == meta[0]['commit'] for m in meta)
raw = {label: json.loads((a.root/label/'metrics.json').read_text()) for label in labels}
out = {'scope': 'this L40S/sm89 only', 'commit': meta[0]['commit'], 'n': 3,
       'labels': labels, 'same_graph': True, 'points': [], 'adjacent_drops': {}}
ids = {}
for i, it in enumerate((64, 128, 256, 512)):
    for path in ('single', 'batch'):
        recalls = [raw[label][i][path+'_recall_at10'] for label in labels]
        speeds = [raw[label][i][path+'_qps'] for label in labels]
        pairwise = []
        for label in labels:
            row = raw[label][i]
            assert row['itopk_size'] == it and row['search_width'] == 1 and row['returned_k'] == 64
            assert row['observed_gpu_queries'] == 10000 and row['observed_gpu_errors'] == 0
            assert row['batch_observed_gpu_dispatches'] == 50 and row['batch_observed_gpu_errors'] == 0
            assert row['batch_observed_fallbacks'] == 0
            ids[label, it, path] = np.load(a.root/label/f'itopk{it}-{path}-ids.npy')
        for x, y in itertools.combinations(labels, 2):
            pairwise.append({'runs': [x, y], **helper.compare_ids(ids[x,it,path], ids[y,it,path], truth)})
        out['points'].append({'itopk_size': it, 'path': path,
            'recall_values': recalls, 'recall_mean': statistics.mean(recalls),
            'recall_min': min(recalls), 'recall_max': max(recalls), 'recall_std': statistics.stdev(recalls),
            'qps_mean': statistics.mean(speeds),
            'mean_run_pair_jaccard': statistics.mean(v['mean_query_jaccard'] for v in pairwise),
            'pairwise_ids': pairwise})
for path in ('single', 'batch'):
    points = [v for v in out['points'] if v['path'] == path]
    out['adjacent_drops'][path] = [
        {'from_itopk': prev['itopk_size'], 'at_itopk': cur['itopk_size'],
         'mean_delta': cur['recall_mean'] - prev['recall_mean'],
         'observed_ranges_nonoverlapping': cur['recall_max'] < prev['recall_min']}
        for prev, cur in zip(points, points[1:]) if cur['recall_mean'] < prev['recall_mean']]
out['single_vs_batch_ids'] = {
    str(it): [{'run': label, **helper.compare_ids(ids[label,it,'single'], ids[label,it,'batch'], truth)}
              for label in labels] for it in (64,128,256,512)}
(a.root/'issue144-aggregate.json').write_text(json.dumps(out, indent=2)+'\n')
fields = ['itopk_size','path','recall_mean','recall_min','recall_max','recall_std','qps_mean','mean_run_pair_jaccard']
with (a.root/'issue144-aggregate.csv').open('w',newline='') as stream:
    writer=csv.DictWriter(stream,fieldnames=fields)
    writer.writeheader()
    writer.writerows({k:v for k,v in row.items() if k in fields} for row in out['points'])
print(json.dumps({k:v for k,v in out.items() if k != 'single_vs_batch_ids'},indent=2))

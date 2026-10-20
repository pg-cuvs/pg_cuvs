#!/usr/bin/env python3
import argparse
import importlib.util
import itertools
import json
from pathlib import Path
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--truth', type=Path, required=True)
parser.add_argument('--helper', type=Path, required=True)
args = parser.parse_args()
labels = ['baseline_control1', 'baseline_control2', 'baseline_control3']
spec = importlib.util.spec_from_file_location('feature_check', args.helper)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
metas = {label: json.loads((args.root/label/'metadata.json').read_text()) for label in labels}
metrics = {label: {str(row['k']): row for row in json.loads((args.root/label/'metrics.json').read_text())} for label in labels}
first = metas[labels[0]]
for label in labels:
    if (args.root.parent/f'{label}.exit').read_text().strip() != '0':
        raise RuntimeError(f'{label} did not complete successfully')
    for field in ('graph', 'query_bytes_sha256', 'truth_bytes_sha256', 'commit', 'warming'):
        if metas[label][field] != first[field]:
            raise RuntimeError(f'{label}: comparison invariant differs: {field}')
truth = np.load(args.truth, mmap_mode='r')[:first['queries'], :10]
out = {field: first[field] for field in ('graph', 'query_bytes_sha256', 'truth_bytes_sha256', 'commit', 'warming')}
out.update(labels=labels, sample_count=3, threshold_rule='absolute difference to control mean <= observed max-min span; not a confidence interval', points={})
for k in (64,128):
    point = {}
    for field in ('recall_at10','p50_ms','qps'):
        values = [metrics[label][str(k)][field] for label in labels]
        point[field] = {'values':values, 'mean':float(np.mean(values)), 'min':min(values), 'max':max(values), 'std':float(np.std(values,ddof=1))}
    pairs = []
    for a,b in itertools.combinations(labels,2):
        ai=np.load(args.root/a/f'k{k}-ids.npy');bi=np.load(args.root/b/f'k{k}-ids.npy')
        pairs.append({'runs':[a,b], **helper.compare_ids(ai,bi,truth)})
    point['pairwise_ids']=pairs
    point['mean_pairwise_query_jaccard']=float(np.mean([p['mean_query_jaccard'] for p in pairs]))
    out['points'][str(k)] = point
out['original_restart_diagnostic']={}
for k in (64,128):
    ai=np.load(args.root/'baseline_initial'/f'k{k}-ids.npy')
    bi=np.load(args.root/'baseline_reload'/f'k{k}-ids.npy')
    out['original_restart_diagnostic'][str(k)]=helper.compare_ids(ai,bi,truth)
target=args.root/'baseline-noise.json'
target.write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))

import hashlib
import json
import math
from pathlib import Path
import sys

root, arm = Path(sys.argv[1]), sys.argv[2]
meta = json.loads((root/'metadata.json').read_text())
metrics = json.loads((root/'metrics.json').read_text())
assert meta['commit'] == '0ff9f7c9fac4c20b8256dfc20ea3b0e604f7dd28'
assert meta['queries'] == 10000 and len(metrics) == 2
assert {p['k'] for p in metrics} == {64,128}
assert meta['rmm_pool_mb'] == ('0' if arm == 'zero' else '512')
assert meta['search_settings']['cuvs.itopk_size'] == '0'
assert meta['search_settings']['cuvs.search_width'] == '0'
for point in metrics:
    assert point['observed_gpu_queries'] == 10000 and point['observed_gpu_errors'] == 0
    assert point['observed_search_mode'] == 'gpu_cagra'
    assert point['recall_at10_within_control_span']
    assert 0 <= point['recall_at10'] <= 1
    assert math.isfinite(point['p50_ms']) and point['p50_ms'] > 0
    if arm == 'zero':
        assert point['p50_ms_within_control_span']
record = {'arm': arm, 'passed': True, 'points': [64,128], 'queries_each': 10000,
          'metrics_sha256': hashlib.sha256((root/'metrics.json').read_bytes()).hexdigest(),
          'zero_recall_and_latency_frozen_gate': arm == 'zero',
          'default_recall_frozen_gate': arm == 'default'}
(root/'quality-gate.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record))

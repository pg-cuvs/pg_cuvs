import hashlib
import json
import platform
import shutil
import sys
from pathlib import Path

assert platform.node() == 'brev-v6nc08sjs'
root = Path('/home/ubuntu/index-accel-validation')
out = root/'rmm-quota-production-20261021'
owner = json.loads((root/'owner.json').read_text())
assert owner['vm_id'] == 'v6nc08sjs' and owner['vm_name'] == 'pg-cuvs-index-accel-20261020'
expected = json.loads((root/'results/baseline-noise.json').read_text())['graph']
snapshot = json.loads((root/'graph-snapshot/manifest.json').read_text())
assert snapshot['graph'] == expected
record = {}
for name, info in expected['files'].items():
    for parent in [root/'graph-snapshot', Path('/tmp/cuvs_indexes')]:
        path = parent/name
        if not path.exists() and sys.argv[1] == 'prepare' and parent == Path('/tmp/cuvs_indexes'):
            parent.mkdir(exist_ok=True)
            shutil.copy2(root/'graph-snapshot'/name, path)
        with path.open('rb') as stream:
            h = hashlib.sha256()
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
                h.update(block)
            actual = {'bytes': path.stat().st_size, 'sha256': h.hexdigest()}
        assert actual == info, (str(path), actual, info)
        record[str(path)] = actual
(out/('graph-'+sys.argv[1]+'.json')).write_text(json.dumps(record, indent=2)+'\n')
print('Original graph and persistent snapshot verified:', sys.argv[1])

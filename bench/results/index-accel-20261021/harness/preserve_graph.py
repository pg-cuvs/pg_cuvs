#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import platform
import shutil

assert platform.node() == 'brev-v6nc08sjs'
root = Path('/home/ubuntu/index-accel-validation')
baseline = json.loads((root/'results/baseline-noise.json').read_text())
target = root/'graph-snapshot'
target.mkdir(exist_ok=False)
records = {}
for name, info in baseline['graph']['files'].items():
    source = Path('/tmp/cuvs_indexes')/name
    dest = target/name
    shutil.copy2(source, dest)
    digest = hashlib.sha256()
    with dest.open('rb') as stream:
        for block in iter(lambda: stream.read(8*1024*1024), b''):
            digest.update(block)
    actual = {'bytes':dest.stat().st_size,'sha256':digest.hexdigest()}
    assert actual == info, (name,actual,info)
    records[name] = actual
(target/'manifest.json').write_text(json.dumps({'graph':baseline['graph'],'verified_copies':records},indent=2)+'\n')
print(json.dumps(records,indent=2))

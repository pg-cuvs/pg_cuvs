"""Real small fbin corpus and exact truth for the local CPU reference fixture."""
import json
from pathlib import Path
import struct
import numpy as np

root = Path('/home/ubuntu/index-accel-validation')
data = Path('/home/ubuntu/data')
data.mkdir(exist_ok=True)
corpus = np.random.default_rng(817).normal(size=(256,768)).astype('float32')
corpus /= np.linalg.norm(corpus,axis=1,keepdims=True)
queries = corpus[:8].copy()
for name, values in [('corpus.fbin',corpus),('queries_10k.fbin',queries)]:
    with (data/name).open('wb') as stream:
        stream.write(struct.pack('<ii',*values.shape))
        stream.write(values.astype('<f4').tobytes())
truth = np.argsort(np.sum((queries[:,None,:]-corpus[None,:,:])**2,axis=2),axis=1)[:,:10]
np.save(data/'gt_256.npy',truth.astype('int64'))
(root/'owner.json').write_text(json.dumps({'vm_name':'pg-cuvs-index-accel-local-fixture',
    'vm_id':'local-cpu-shim-fixture','fixture':True,'hardware_GPU':False})+'\n')
print('Real local fixture: 256 x 768 corpus, 8 queries, exact top10 truth; CPU shim only')

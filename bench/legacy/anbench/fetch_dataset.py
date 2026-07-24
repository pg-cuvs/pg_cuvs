#!/usr/bin/env python3
"""
fetch_dataset.py - download Cohere Wikipedia 768d embeddings (PRE-COMPUTED; we
never run an embedding model) and materialize normalized .fbin slices for the
ANN benchmark. Runs ON THE VM (not locally).

Output (under --out-dir, default ~/anbench/data):
  corpus_10m.fbin   first 10,000,000 rows, L2-normalized
  corpus_1m.fbin    first 1,000,000 rows (prefix of the above)
  queries_10k.fbin  rows 10,000,000 .. 10,010,000, held out, L2-normalized

.fbin format (big-ann standard): int32 n, int32 dim, then n*dim float32 row-major.

Why normalize: pg_cuvs searches under L2 regardless of opclass, and for unit-norm
vectors L2-NN ranking == cosine-NN ranking. Normalizing once here makes every
system (all using L2 / vector_l2_ops) metric-consistent on this cosine dataset.

Streams parquet shards one at a time and deletes each after processing, so peak
disk stays near the output size, not the full ~35M-row repo.
"""
import argparse
import os
import shutil
import struct
import sys

# Use classic (non-xet) downloads so we can delete each shard's real file and
# keep peak disk near the output size, not the whole repo + xet blob cache.
os.environ.setdefault("HF_HUB_DISABLE_XET", "1")

import numpy as np

# Public, no token. Cohere embed-multilingual-v3.0, 1024d. We take the
# English subset only (en/ shards) for a clean single-language distribution.
# (The older 22-12 en 768d set is gated and needs an HF token.)
REPO_ID = "Cohere/wikipedia-2023-11-embed-multilingual-v3"
LANG_PREFIX = "en/"
EMB_COL = "emb"
DIM = 1024


def log(msg):
    print(f"[fetch] {msg}", flush=True)


def fbin_write_header(path, n, dim):
    with open(path, "wb") as f:
        f.write(struct.pack("<ii", n, dim))


def fbin_shape(path):
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        n, dim = struct.unpack("<ii", f.read(8))
    expected = 8 + n * dim * 4
    return (n, dim, os.path.getsize(path) == expected)


def normalize_rows(a):
    a = np.ascontiguousarray(a, dtype=np.float32)
    norms = np.linalg.norm(a, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    return a / norms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=os.path.expanduser("~/anbench/data"))
    ap.add_argument("--n-corpus", type=int, default=5_000_000)
    ap.add_argument("--n-queries", type=int, default=10_000)
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    corpus_path = os.path.join(args.out_dir, "corpus.fbin")
    queries_path = os.path.join(args.out_dir, "queries_10k.fbin")
    scratch = os.path.join(args.out_dir, ".dl")

    want_corpus = (args.n_corpus, DIM, True)
    want_queries = (args.n_queries, DIM, True)
    if fbin_shape(corpus_path) == want_corpus and fbin_shape(queries_path) == want_queries:
        log("outputs already present with correct shape; nothing to do")
        return 0

    # Lazy imports so --help works without the heavy deps.
    import pyarrow.parquet as pq
    from huggingface_hub import HfApi, hf_hub_download

    api = HfApi()
    files = [f for f in api.list_repo_files(REPO_ID, repo_type="dataset")
             if f.endswith(".parquet") and f.startswith(LANG_PREFIX)]
    files.sort()
    if not files:
        log(f"ERROR: no parquet files found in {REPO_ID}")
        return 1
    log(f"{len(files)} parquet shards in {REPO_ID}")

    total_needed = args.n_corpus + args.n_queries
    fbin_write_header(corpus_path, args.n_corpus, DIM)
    fbin_write_header(queries_path, args.n_queries, DIM)
    cf = open(corpus_path, "ab")
    qf = open(queries_path, "ab")

    written = 0  # rows consumed from the global stream
    for shard in files:
        if written >= total_needed:
            break
        local = hf_hub_download(REPO_ID, shard, repo_type="dataset", local_dir=scratch)
        try:
            col = pq.read_table(local, columns=[EMB_COL]).column(EMB_COL).combine_chunks()
            # list<float>[768] or fixed_size_list<float>[768]: flatten the child
            # values and reshape (fast, no per-row Python objects).
            flat = np.asarray(col.values.to_numpy(zero_copy_only=False), dtype=np.float32)
            if flat.size % DIM != 0:
                log(f"ERROR: shard {shard} flat size {flat.size} not divisible by {DIM}")
                return 1
            emb = flat.reshape(-1, DIM)
            take = min(len(emb), total_needed - written)
            emb = normalize_rows(emb[:take])
            # route by global index: [0, n_corpus) -> corpus, rest -> queries
            gstart = written
            gend = written + take
            c_lo, c_hi = max(gstart, 0), min(gend, args.n_corpus)
            if c_hi > c_lo:
                cf.write(np.ascontiguousarray(emb[c_lo - gstart:c_hi - gstart]).tobytes())
            q_lo, q_hi = max(gstart, args.n_corpus), min(gend, total_needed)
            if q_hi > q_lo:
                qf.write(np.ascontiguousarray(emb[q_lo - gstart:q_hi - gstart]).tobytes())
            written += take
            log(f"shard {shard}: written {written}/{total_needed}")
        finally:
            # remove the shard's real file + any hf cache so peak disk stays low
            try:
                os.remove(local)
            except OSError:
                pass
            shutil.rmtree(os.path.join(scratch, ".cache"), ignore_errors=True)

    cf.close()
    qf.close()
    if written < total_needed:
        log(f"ERROR: stream exhausted at {written} < {total_needed} rows")
        return 1

    # Runners read the first N rows from corpus_10m.fbin via offset slicing
    # (no separate 1M/5M files — keeps peak disk near one corpus copy).
    for p in (corpus_path, queries_path):
        log(f"  {p}: shape={fbin_shape(p)}")
    log("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())

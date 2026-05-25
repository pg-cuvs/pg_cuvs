#!/bin/bash
# run_all.sh - run the full ANN benchmark for one corpus size N across all
# systems, each in its own conda env. Run ON THE VM from ~/pg_cuvs.
#
#   N=1000000 KS=10,100 bash infra/anbench/run_all.sh
#
# Tiers:  Tier A (SQL): pg_cuvs, pgvector hnsw, pgvector ivfflat
#         Tier B (lib): raw cuvs CAGRA, faiss-gpu, faiss-cpu
# GPU systems are skipped automatically if N*dim*4 exceeds a VRAM budget.

set -u
N=${N:-1000000}
KS=${KS:-10,100}
DATA=${DATA:-$HOME/anbench/data}
OUT=${OUT:-$HOME/pg_cuvs/design/anbench/results}
GPU_VRAM_MB=${GPU_VRAM_MB:-40000}
DAEMON_VRAM_MB=${DAEMON_VRAM_MB:-38000}

CORPUS=$DATA/corpus.fbin
Q=$DATA/queries_10k.fbin
GT=$DATA/gt_${N}.npy
RUN=$OUT/run_N${N}.jsonl
mkdir -p "$OUT"
cd "$HOME/pg_cuvs"

DIM=$(python3 -c "import struct;print(struct.unpack('<ii',open('$CORPUS','rb').read(8))[1])")
GPU_FITS=$(python3 -c "print(1 if $N*$DIM*4/1e6 < $GPU_VRAM_MB*0.92 else 0)")
echo "[run_all] N=$N dim=$DIM KS=$KS gpu_fits=$GPU_FITS RUN=$RUN"

act() { source ~/miniforge3/bin/activate "$1"; }
step() { echo; echo "===== $* ====="; }

# --- ground truth (cuvs_py) ---
act cuvs_py
if [ ! -f "$GT" ]; then
    step "build GT N=$N"
    python infra/anbench/build_gt.py --corpus "$CORPUS" --queries "$Q" --n "$N" --k 100 --out "$GT" || exit 1
fi

# --- Tier A: pgvector (CPU, in-PG) ---
for sys in hnsw ivfflat; do
    step "pgvector $sys N=$N"
    python infra/anbench/run_pg.py --corpus "$CORPUS" --queries "$Q" --gt "$GT" \
        --n "$N" --system "$sys" --out "$RUN" --ks "$KS" || echo "[run_all] WARN $sys failed"
done

# --- Tier A: pgvector exact (seqscan, no index) = in-PG brute-force baseline ---
step "pgvector exact (seqscan) N=$N"
python infra/anbench/run_pg.py --corpus "$CORPUS" --queries "$Q" --gt "$GT" \
    --n "$N" --system exact --out "$RUN" --ks "$KS" || echo "[run_all] WARN exact failed"

# --- Tier A: pg_cuvs (GPU via sidecar) ---
if [ "$GPU_FITS" = "1" ]; then
    step "pg_cuvs N=$N (raise daemon --max-vram-mb=$DAEMON_VRAM_MB)"
    sudo sed -i "s/--max-vram-mb [0-9]*/--max-vram-mb $DAEMON_VRAM_MB/" \
        /etc/systemd/system/pg-cuvs-server.service 2>/dev/null || true
    sudo systemctl daemon-reload 2>/dev/null || true
    sudo systemctl restart pg-cuvs-server; sleep 3
    python infra/anbench/run_pg.py --corpus "$CORPUS" --queries "$Q" --gt "$GT" \
        --n "$N" --system pg_cuvs --out "$RUN" --ks "$KS" || echo "[run_all] WARN pg_cuvs failed (VRAM?)"
else
    echo "[run_all] SKIP pg_cuvs: N=$N dim=$DIM exceeds GPU VRAM budget (OOM finding)"
fi

# --- Tier B: raw cuvs CAGRA (cuvs_py) ---
if [ "$GPU_FITS" = "1" ]; then
    step "raw cuvs CAGRA N=$N"
    python infra/anbench/run_cuvs.py --corpus "$CORPUS" --queries "$Q" --gt "$GT" \
        --n "$N" --out "$RUN" --ks "$KS" || echo "[run_all] WARN cuvs failed"
else
    echo "[run_all] SKIP cuvs: exceeds GPU VRAM budget (OOM finding)"
fi

# --- Tier B: faiss-gpu ---
if [ "$GPU_FITS" = "1" ]; then
    step "faiss-gpu N=$N"
    act faiss_gpu
    python infra/anbench/run_faiss.py --corpus "$CORPUS" --queries "$Q" --gt "$GT" \
        --n "$N" --out "$RUN" --mode gpu --ks "$KS" || echo "[run_all] WARN faiss-gpu failed"
else
    echo "[run_all] SKIP faiss-gpu: exceeds GPU VRAM budget (OOM finding)"
fi

# --- Tier B: faiss-cpu ---
step "faiss-cpu N=$N"
act faiss_cpu
python infra/anbench/run_faiss.py --corpus "$CORPUS" --queries "$Q" --gt "$GT" \
    --n "$N" --out "$RUN" --mode cpu --ks "$KS" || echo "[run_all] WARN faiss-cpu failed"

step "DONE N=$N -> $RUN"

#!/usr/bin/env bash
# run.sh — single entry point for the benchmark protocol harness.
#
# Contract: bench/protocol/CONTRACT.md. Config comes from env (CONTRACT §2).
# This script writes ONLY under results/protocol/ — git push is the workflow's job.
#
# STATE: dispatch skeleton. The actual measurement (resource sampling + engine
# build/query + CONTRACT §6 CSV row) is delegated to observe.py + engines/<config>.sh
# at the SEAM marked below. Until observe.py lands, PGCUVS_CPU_SHIM=1 exercises the
# plumbing end-to-end (enumeration, resume, dry-run, terminal marker, CSV shape).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="$HERE/lib"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
RESULTS_ROOT="${PGCUVS_RESULTS_ROOT:-$REPO_ROOT/bench/results/protocol}"

# ── env (CONTRACT §2) ────────────────────────────────────────────────────────
STAGE="${PGCUVS_STAGE:?PGCUVS_STAGE required (A|B|C|D)}"
MODULE="${PGCUVS_MODULE:-physics}"
CELLS="${PGCUVS_CELLS:?PGCUVS_CELLS required (resolve_cells grammar)}"
CONFIGS="${PGCUVS_CONFIGS:-forced-hnsw,forced-cuvs,auto}"
BASELINE="${PGCUVS_BASELINE:-same-box}"
DATASET="${PGCUVS_DATASET:-cohere-1m}"
REPS="${PGCUVS_REPS:-5}"
RUN_ID="${PGCUVS_RUN_ID:-local-$(date -u +%Y%m%dT%H%M%SZ)}"
DRY_RUN="${PGCUVS_DRY_RUN:-0}"
RESUME="${PGCUVS_RESUME:-0}"
CPU_SHIM="${PGCUVS_CPU_SHIM:-0}"
COST_MODEL_VERSION="${PGCUVS_COST_MODEL_VERSION:-unset}"
RUNTIME_ROUTING_VERSION="${PGCUVS_RUNTIME_ROUTING_VERSION:-unset}"

OUTDIR="$RESULTS_ROOT/$STAGE"
CSV="$OUTDIR/$RUN_ID.csv"
PROGRESS="$OUTDIR/$RUN_ID.progress"
MANIFEST="$OUTDIR/$RUN_ID.manifest.json"
mkdir -p "$OUTDIR"

# CONTRACT §6 schema — SINGLE SOURCE here. MUST stay identical to observe.py's
# writer when it lands (reconcile to one definition then).
CSV_HEADER="run_id,date,stage,phase,cell_id,config,system,system_version,system_commit,index_type,N,dim,k,recall_target,dataset,query_set_id,seed,clients,warm_state,build_s,qps,p50_us,p95_us,p99_us,p999_us,avg_latency_us,recall_at_k,peak_vram_mb,peak_rss_mb,cpu_core_s,gpu_s,energy_j,disk_bytes_written,wal_bytes,index_bytes_vram,index_bytes_host,index_bytes_disk,instance_type,price_usd_hr,usd_per_1m_queries,reps,agg_method,dispersion,gt_method,cost_model_version,runtime_routing_version,selectivity,correlation,filter_mode,stream_op,ops_done,delta_rows,params_json,notes"

log(){ echo "[run.sh] $*" >&2; }

# ── resolve plan = cells × configs ───────────────────────────────────────────
mapfile -t CELL_IDS < <("$LIB/resolve_cells.sh" "$CELLS")
IFS=',' read -ra CONFIG_LIST <<< "$CONFIGS"
PLAN=()
for c in "${CELL_IDS[@]}"; do
  for cfg in "${CONFIG_LIST[@]}"; do PLAN+=("$c|$cfg"); done
done
TOTAL=${#PLAN[@]}

if [ "$DRY_RUN" = "1" ]; then
  log "DRY RUN — stage=$STAGE module=$MODULE baseline=$BASELINE dataset=$DATASET reps=$REPS"
  log "resolved ${#CELL_IDS[@]} cells × ${#CONFIG_LIST[@]} configs = $TOTAL measurements:"
  printf '%s\n' "${PLAN[@]}"
  echo "PGCUVS_RESULT: status=OK cells_done=0/$TOTAL (dry-run)"
  exit 0
fi

# ── init outputs + resume ────────────────────────────────────────────────────
[ -f "$CSV" ] || echo "$CSV_HEADER" > "$CSV"
touch "$PROGRESS"
declare -A DONE=()
if [ "$RESUME" = "1" ]; then
  while IFS= read -r line; do [ -n "$line" ] && DONE["$line"]=1; done < "$PROGRESS"
  log "resume: ${#DONE[@]} measurements already complete — skipping those"
fi

write_manifest(){
  # SEAM: observe.py owns the rich manifest (pg_settings non-default dump, pg_cuvs
  # sha / cuVS / CUDA / driver versions, gt_method, terminal status — CONTRACT §3).
  # Placeholder until observe.py lands: env snapshot only.
  cat > "$MANIFEST" <<EOF
{
  "run_id": "$RUN_ID", "stage": "$STAGE", "module": "$MODULE",
  "baseline": "$BASELINE", "dataset": "$DATASET", "reps": $REPS,
  "cpu_shim": $CPU_SHIM,
  "cost_model_version": "$COST_MODEL_VERSION",
  "runtime_routing_version": "$RUNTIME_ROUTING_VERSION",
  "started": "$(date -u +%Y-%m-%dT%H:%M:%SZ)", "total": $TOTAL,
  "_note": "placeholder manifest — observe.py replaces with full env/version dump"
}
EOF
}
write_manifest

measure(){
  local cell_id="$1" config="$2"
  # ══════════════════════════ SEAM ══════════════════════════
  # Real path (observe.py landed):
  #   observe.py samples nvidia-smi (device-delta VRAM) + /usr/bin/time + power.draw,
  #   invokes engines/<config>.sh to build+query under iso-recall, and appends a
  #   CONTRACT §6 row to $CSV. run.sh stays the dispatcher; observe.py owns CSV+sampling.
  # Until then, CPU_SHIM emits a syntactically valid placeholder row so the plumbing
  # is end-to-end testable without GPU/PG.
  # ═══════════════════════════════════════════════════════════
  if [ "$CPU_SHIM" = "1" ]; then
    local n d k r
    n="${cell_id#N}"; n="${n%%_*}"
    d="$(sed -n 's/.*_d\([0-9]*\)_.*/\1/p' <<<"$cell_id")"
    k="$(sed -n 's/.*_k\([0-9]*\)_.*/\1/p' <<<"$cell_id")"
    r="${cell_id##*_r}"
    # one row per CONTRACT §6 column order; zeros/empties for unmeasured fields
    printf '%s,%s,%s,query,%s,%s,shim,0,0,shim,%s,%s,%s,%s,%s,q,0,1,warm,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,shim,0,0,%s,shim,0,shim,%s,%s,,,,,,,{},cpu-shim-placeholder\n' \
      "$RUN_ID" "$(date -u +%F)" "$STAGE" "$cell_id" "$config" \
      "$n" "$d" "$k" "$r" "$DATASET" "$REPS" \
      "$COST_MODEL_VERSION" "$RUNTIME_ROUTING_VERSION" >> "$CSV"
    return 0
  fi
  log "ERROR: real measurement path not wired yet (observe.py pending). cell=$cell_id config=$config"
  return 3
}

# ── main loop: per-cell-per-config atomic unit (CONTRACT §3) ──────────────────
status=OK; done_n=0
for item in "${PLAN[@]}"; do
  if [ -n "${DONE[$item]:-}" ]; then done_n=$((done_n+1)); continue; fi
  cell_id="${item%%|*}"; config="${item##*|}"
  if measure "$cell_id" "$config"; then
    echo "$item" >> "$PROGRESS"
    sync -f "$PROGRESS" 2>/dev/null || sync   # checkpoint: durable before next cell
    done_n=$((done_n+1))
  else
    status=FAIL
    log "measurement FAILED at $item — stopping (resume with PGCUVS_RESUME=1)"
    break
  fi
done

# terminal marker (CONTRACT §3) — stdout last line, for webhook/log diagnosis
echo "PGCUVS_RESULT: status=$status cells_done=$done_n/$TOTAL"
[ "$status" = OK ]

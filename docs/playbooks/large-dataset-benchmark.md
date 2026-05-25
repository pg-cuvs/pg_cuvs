# Playbook: 대규모 데이터셋 벤치마크

Phase 1.5 운영 기준선 고정을 위한 벤치마크 절차.
JIT threshold 결정, VRAM 예산 설정, fallback 정책 검증의 근거 데이터를 수집한다.
PLAN.md §5 참조.

---

## 1. 증상 (Symptoms)

이 playbook은 장애 대응이 아니라 측정 절차다.
다음 상황에서 실행한다:

- Phase 1.5 완료 기준으로 1M/1536d 이상 벤치마크가 필요할 때.
- `EXPLAIN (ANALYZE)`에 `JIT:` 섹션이 나타나 latency variance가 의심될 때.
- VRAM 예산(`cuvs.max_vram_mb`) 또는 `--max-vram-mb` 값을 결정해야 할 때.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# GPU 메모리 현황
ssh $GCP_VM "nvidia-smi --query-gpu=memory.total,memory.used,memory.free \
  --format=csv,noheader,nounits"

# daemon 보유 인덱스 및 VRAM 사용량 (journal 기준)
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server --no-pager \
  | grep -E 'loaded index|built index'"

# 백엔드별 CUDA context 소유 여부 확인 (pg_cuvs_server 외 프로세스가 GPU를 잡으면 ADR-002 위반)
ssh $GCP_VM "nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
  --format=csv,noheader"
```

---

## 3. 원인 분기 (Cause branches)

이 playbook의 분기는 장애 원인이 아닌 측정 결과 해석이다.

| 관측 | 의미 | 다음 단계 |
|------|------|-----------|
| `EXPLAIN`에 `JIT:` 섹션 없음 | 정상. threshold 변경 불필요 | 완료 |
| `JIT:` 섹션 있고 p95/p99 안정 | JIT이 무해하거나 이득 | threshold sweep 생략 가능 |
| `JIT:` 섹션 있고 p95/p99 스파이크 | JIT이 vector-search latency를 해침 | jit-threshold-sweep.md 실행 |
| `nvidia-smi`에 PG 백엔드가 CUDA context 소유 | 아키텍처 위반 (ADR-002) | 즉시 에스컬레이션 |
| fallback count > 0 (`OOM_FALLBACK`) | VRAM 부족 | `--max-vram-mb` 조정 또는 인덱스 수 감소 |

---

## 4. 복구 절차 (Recovery steps)

### 4-1. 데이터 생성

Dataset tiers (PLAN.md §5): Small=10K, Medium=1M, Large=10M.
Dimensions: 384, 768, 1536. Query k: 10, 100, 1000.

```sql
-- Small: 10K x dim=384
CREATE TABLE bench_10k_384 (id bigint, v vector(384));
INSERT INTO bench_10k_384
  SELECT i,
    (SELECT array_agg(random()::float4)::vector(384)
     FROM generate_series(1,384))
  FROM generate_series(1, 10000) i;

-- Medium: 1M x dim=384  (VM에서 직접 실행 권장)
CREATE TABLE bench_1m_384 (id bigint, v vector(384));
INSERT INTO bench_1m_384
  SELECT i,
    (SELECT array_agg(random()::float4)::vector(384)
     FROM generate_series(1,384))
  FROM generate_series(1, 1000000) i;
```

dim=768, dim=1536 테이블은 같은 패턴으로 생성한다.

### 4-2. 인덱스 빌드 시간 측정

```sql
\timing on
CREATE INDEX cagra_10k_384 ON bench_10k_384 USING cagra (v vector_l2_ops);
-- \timing 출력 -> "CREATE INDEX build time" 기록
\timing off
```

```bash
# 빌드 직후 daemon journal에서 VRAM 사용량 확인
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server --no-pager | grep 'built index' | tail -5"
# 출력 예: built index 16384/16392 (1000000 vecs, 512 MB VRAM)

# .cagra + .tids 아티팩트 크기
ssh $GCP_VM "ls -lh /tmp/cuvs_indexes/"
```

### 4-3. 검색 latency 측정 (cold/warm planning, k=10/100/1000)

```sql
-- cold planning: 새 세션에서 첫 번째 EXPLAIN
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,0.3]'::vector(384)
  LIMIT 10;
-- "Planning Time", "Execution Time", "JIT:" 섹션 여부 기록

-- warm planning: 같은 세션, 두 번째 실행
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,0.3]'::vector(384)
  LIMIT 10;
```

마지막 GPU search latency (daemon 보고값):

```sql
SELECT pg_cuvs_last_search_latency_us();
```

### 4-4. daemon reload 시간 측정

```bash
# daemon 재시작 후 reload 시간
ssh $GCP_VM "time sudo systemctl restart pg-cuvs-server"

# 각 인덱스의 reload 확인
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server --no-pager | grep 'loaded index'"
```

### 4-5. fallback count 확인

Phase 2에서 `pg_stat_gpu_search`가 추가되면 해당 뷰를 사용한다.
현재는 daemon journal로 확인한다.

```bash
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server --no-pager \
  | grep -iE 'fallback|OOM|evict|circuit'"
```

### 4-6. 결과 기록 형식

| 항목 | 10K/384 | 1M/384 | 1M/1536 | 10M/384 |
|------|---------|--------|---------|---------|
| BUILD time (s) | | | | |
| backend peak RSS (MB) | | | | |
| daemon VRAM (MB) | | | | |
| .cagra size (MB) | | | | |
| .tids size (MB) | | | | |
| reload time (s) | | | | |
| cold planning (ms) | | | | |
| warm planning (ms) | | | | |
| exec p50 (ms) | | | | |
| exec p95 (ms) | | | | |
| exec p99 (ms) | | | | |
| fallback count | | | | |
| JIT: section | yes/no | yes/no | yes/no | yes/no |

---

## 5. 검증 명령 (Verification commands)

```bash
# daemon이 단독으로 GPU를 점유하는지 확인 (PG 백엔드 0 MB여야 함)
ssh $GCP_VM "nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
  --format=csv,noheader"
# pg_cuvs_server 프로세스만 나와야 함
```

```sql
-- 검색 결과 정확도 (id=1이 자기 자신의 최근접 이웃이어야 함)
SELECT id FROM bench_10k_384
ORDER BY v <-> (SELECT v FROM bench_10k_384 WHERE id = 1)
LIMIT 1;
-- 출력: 1
```

---

## 6. Escalation 기준 (When to escalate)

- `nvidia-smi`에서 PG 백엔드 프로세스가 CUDA context를 소유하면: 즉시 에스컬레이션. ADR-002 위반.
- 1M 벤치마크에서 `cuvs_ambuild()` 중 backend OOM
  (`pg_cuvs: out of memory accumulating index vectors`): `work_mem` 조정은 효과 없다
  (malloc 사용). PLAN.md Phase 2 §5 streaming handoff로 에스컬레이션.
- `EXPLAIN (ANALYZE)`에 `JIT:` 섹션이 있고 p95 latency가 p50의 3배 이상이면:
  jit-threshold-sweep.md를 실행한다. 측정 없이 `jit = off`를 적용하지 않는다 (ADR-018).

---

## 7. How to run (harness)

위 §4의 수동 절차는 `infra/scripts/benchmark.sh`로 자동화되어 있다.
로컬 노트북에는 toolchain/daemon이 없으므로 **VM에서 leader가 실행**한다.

```bash
# Default sanity 실행 (10000 x 384, k=10, M=100 queries). 빠르게 동작 확인.
make gpu-bench

# PLAN 완료 게이트 실행 (1M x 1536d, VRAM-stress). 명시적 타깃.
make gpu-bench-1m

# 임의 크기: N/DIM/K/M 를 넘겨 특정 셀만 실행.
make gpu-bench N=1000000 DIM=384 K=100
```

출력은 `make gpu-bench`가 `design/bench_<timestamp>.log`로 tee 한다.
모든 metric은 `metric: value` 형태이며 끝에 `[bench] SUMMARY` 블록으로 모인다:
`build_time_s`, `cagra_bytes`, `tids_bytes`, `vram_used_mb_after_build`,
`cold_planning_ms`, `warm_planning_ms`, `exec_p50_ms`/`p95`/`p99`,
`fallbacks`, `reload_time_s`, `compute_apps`, 그리고 핵심 게이트 datum인
`jit_section: yes/no`.

**핵심:** `jit_section: yes`이면 §3 분기에 따라 p95/p99를 보고
jit-threshold-sweep.md sweep 필요 여부를 결정한다. `jit_section: no`이면
threshold 변경 불필요.

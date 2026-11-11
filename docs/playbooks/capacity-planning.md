# Playbook: 용량 계획 (capacity-planning)

CAGRA 인덱스 하나가 GPU에 차지하는 VRAM, 디스크 artifact 크기, delta 크기를
사전에 계산해 `--max-vram-mb`와 `shard_count`를 결정하는 절차.
수치는 `src/pg_cuvs_server.c`의 `estimate_vram_bytes`,
`src/cuvs_util.c cuvs_auto_shard_count`, 실측 결과
(`design/benchmarks/crossover-methodology.md §11·§12·§13·§16`, `bench/results/`)에 근거한다.

---

## 1. 증상 (Symptoms)

이 playbook은 장애 대응이 아닌 사전 계획 절차다. 다음 상황에서 실행한다:

- 새 인덱스(N, dim)를 빌드하기 전에 VRAM이 충분한지 확인하고 싶을 때.
- `pg_stat_gpu_cache`에서 `vram_used_mb`가 `vram_budget_mb`에 근접하고 eviction이
  잦아 shard_count 조정이 필요할 때.
- 멀티 GPU VM 또는 `--max-vram-mb` 설정값을 결정해야 할 때.
- 인덱스 artifact 디스크 요구량(`cuvs.index_dir` 파티션)을 계획할 때.

---

## 2. 확인 명령 (Diagnostic commands)

```sql
-- 현재 GPU별 VRAM 사용/여유
SELECT gpu_device_id, vram_used_mb, vram_budget_mb,
       vram_budget_mb - vram_used_mb AS vram_free_mb,
       resident_count, evictions
  FROM pg_stat_gpu_cache
 ORDER BY gpu_device_id;
```

**기대 출력 (여유 있음):**
```
 gpu_device_id | vram_used_mb | vram_budget_mb | vram_free_mb | resident_count | evictions
---------------+--------------+----------------+--------------+----------------+-----------
             0 |         1638 |          36864 |        35226 |              1 |         0
```
**→ `vram_free_mb` 부족:** 새 인덱스 빌드 전 기존 인덱스 eviction 유도 또는
shard_count 증가 / VM 교체 검토.

```sql
-- 현재 로드된 인덱스별 VRAM 소비
SELECT index_name, dim, n_vecs, vram_bytes,
       round(vram_bytes / 1048576.0) AS vram_mb,
       shard_count, resident, gpu_device_id
  FROM pg_stat_gpu_search
 ORDER BY vram_bytes DESC;
```

```bash
# GPU 물리 VRAM (장치별 총용량)
nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader,nounits
# 기대: 0, NVIDIA A100-SXM4-40GB, 40448  (단위: MiB)
```

```bash
# artifact 디스크 현황
du -sh $PGDATA/cuvs_indexes/ 2>/dev/null || du -sh /tmp/cuvs_indexes/
ls -lh /tmp/cuvs_indexes/ | grep -v '^total'
```

---

## 3. 원인 분기 (Cause branches)

### A. 상주 VRAM 추정과 빌드 admission

`estimate_vram_bytes` 기준(기본 `graph_degree=64`, 차원이 4의 배수일 때):

```
resident_final(bytes) ≈ N × (dim × 4 + graph_degree × 4)
```

| N | dim | VRAM 추정 | 비고 |
|---|-----|-----------|------|
| 100K | 384 | ≈ 171 MiB | 상주 추정치 |
| 1M | 384 | ≈ 1.67 GiB | 상주 추정치 |
| 1M | 1024 | ≈ 4.05 GiB | 상주 추정치 |
| 10M | 384 | ≈ 16.69 GiB | 상주 추정치 |
| 50M | 384 | ≈ 83.45 GiB | 전체 상주 추정치; shard별 admission은 별도 |

> 50M×384 실측(§12): shard_count=4로 corpus 로딩(76.8 GB) 성공 후 데몬 교착 → 강제 종료.
> 해당 구성의 실패 기록이며 다른 구성의 최소 요건은 확정하지 않는다.

이 표는 최종 상주 크기이며 빌드 가능 판정값이 아니다. 현행 데몬은
`admission_needed_bytes(final) = final + ceil(final × scratch_permille / 1000)`을
사용한다. 기본 `scratch_permille=9593`은 특정 A100-80GB의 `managed` 정책에서
검증한 값으로 다른 GPU·정책에는 재보정이 필요하다. RMM pool quota
(`--rmm-pool-max-percent`, 기본 50%), 정책별 workspace/회수 조건, 물리 여유
32 MiB preflight(`PHYS`)도 통과해야 한다. `SHOW cuvs.rmm_pool_status`와
`SELECT pg_cuvs_last_rejection()`으로 거부 사유를 확인한다.

```
per-GPU 예산 = --max-vram-mb (생략 시 물리 VRAM의 90%)
```
`cuvs.build_mem_safety_ratio=0.5`는 PostgreSQL 호스트의 `MemAvailable`에서
빌드 corpus 메모리 상한을 계산할 때 쓰인다. GPU VRAM 예산의 절반이 아니다.

### B. auto shard_count 결정 규칙

`cuvs_auto_shard_count`는 **final-only** 근사치로 shard 수를 고른다.
이후 각 shard는 별도로 scratch를 더한 `needed`로 admission을 받으므로
auto가 shard 수를 반환해도 실제 빌드는 거부될 수 있다.

```
final_approx = N × (dim × 4 + 64)  # auto helper의 기본 degree 근사
if final_approx <= per_gpu_budget:
    shard_count = 1  (unsharded)
else:
    shard_count = ceil(final_approx / per_gpu_budget)
    cap = min(n_gpus, 256, n_vecs / 2)
    if shard_count > cap: build is rejected (no viable auto shard count)
```

`cuvs.shard_count=0`(auto)일 때 위 공식이 적용된다. `want > cap`이면
`cuvs_auto_shard_count()`는 0을 반환하며, 작은 shard 수로 축소해 진행하지 않는다.
단일 GPU VM에서 auto는 근사치가 예산에 들어갈 때만 1(unsharded)이다.
초과하면 cap이 1이므로 0(빌드 거부)을 반환한다.
sharding은 **멀티 GPU VM**에서만 auto가 ≥2를 생성한다.

### C. `--max-vram-mb` 권장값

```
권장 = 물리 VRAM(MiB) × 0.70 ~ 0.80
```

| GPU | 물리 VRAM | 권장 --max-vram-mb |
|-----|-----------|-------------------|
| A100-40GB | 40,448 MiB | 28,000 ~ 32,000 |
| A100-80GB | 81,920 MiB | 57,000 ~ 65,000 |

100% 설정 시 CUDA 내부 컨텍스트 오버헤드로 OOM이 발생할 수 있다.
`cuvs.max_build_mem_mb=0`(auto) 상태의 `build_mem_safety_ratio=0.5`는
PostgreSQL 호스트의 `MemAvailable`에 적용되는 corpus 상한이다.

### D. artifact 디스크 크기 추정

| artifact | 크기 공식 | 예시 (1M×384) | 예시 (1M×1024) |
|----------|----------|----------------|----------------|
| `.cagra` | cuVS 직렬화 결과를 실측 | 빌드 후 확인 | 빌드 후 확인 |
| `.tids` | 32 + N×8 bytes | 1M → 8 MiB | 1M → 8 MiB |
| `.shards` | 40 + shard_count×40 bytes | shard=2 → ~120 B | shard=2 → ~120 B |
| `.delta` (최대) | 32 + max_delta_rows×(8+dim×4) | 10K rows, 384d → ≈ 15 MiB | 10K rows, 1024d → ≈ 41 MiB |
| `.s000.cagra` (sharded) | shard별 cuVS 직렬화 결과를 실측 | 빌드 후 확인 | — |

> `.delta` 최대값은 `cuvs.max_delta_rows`(기본 10000) 기준.
> sharded 인덱스의 `.shards` manifest는 수십 바이트라 무시 가능.
> `.tombstone`/`.stale` 마커는 수 바이트.

---

## 4. 복구 절차 (Recovery steps)

### 단계 1 — 신규 인덱스 VRAM 사전 계산

```
N = 목표 벡터 수
dim = 벡터 차원

resident_final_MiB = N × (dim × 4 + 256) / (1024 × 1024)
```

예:
```
N=5,000,000, dim=768
= 5M × (768×4 + 256) / (1024×1024)
= 5M × 3328 / 1048576
≈ 15.50 GiB
```

```sql
-- 현재 여유와 비교
SELECT gpu_device_id,
       vram_budget_mb - vram_used_mb AS vram_free_mb
  FROM pg_stat_gpu_cache;
-- 이 차이는 상주 예산 여유이며 빌드 admission 통과 판정이 아니다.
```

### 단계 2 — shard_count 및 GPU 구성 결정

```
per_gpu_budget = max_vram_mb  # 생략 시 물리 VRAM의 90%; 다른 상주 인덱스와 workspace 여유는 별도 확인

if final_approx <= per_gpu_budget:
    shard_count = 0 (auto → 1)  # 단일 GPU 충분
else:
    need_shards = ceil(final_approx / per_gpu_budget)
    # shard 수 후보일 뿐. shard별 scratch/RMM/PHYS gate를 따로 확인한다.
    # 50M×384은 별도 실측에서 교착됨. 공식상 분할 가능성은 빌드 성공 증거가 아님.
```

### 단계 3 — `--max-vram-mb` 설정 및 데몬 재시작

```bash
# 현재 systemd unit ExecStart 확인
sudo systemctl cat pg-cuvs-server | grep ExecStart
# 운영자가 실제 GPU 여유와 admission을 고려해 결정한 값을 override에 반영
sudo systemctl edit pg-cuvs-server
# override에서 ExecStart= 빈 줄로 기존 명령을 지우고,
# 원래 인수를 보존한 새 ExecStart= 행에 --max-vram-mb <결정값>을 넣는다.
sudo systemctl daemon-reload
sudo systemctl restart pg-cuvs-server
```

**기대 출력 (journalctl):**
```
[INFO] pg_cuvs_server: GPU 0 (NVIDIA A100-SXM4-40GB): 40448 MB total, budget 28000 MB
[INFO] pg_cuvs_server: listening on /tmp/.s.pg_cuvs
```

### 단계 4 — 인덱스 빌드 및 VRAM 사용 확인

```sql
CREATE INDEX my_cagra ON t USING cagra (v vector_l2_ops);
-- 빌드 후 실제 VRAM 사용량 확인
SELECT index_name, round(vram_bytes/1048576.0) AS vram_mb, n_vecs, dim
  FROM pg_stat_gpu_search WHERE index_name='my_cagra';
```

---

## 5. 검증 명령 (Verification commands)

```sql
-- VRAM 예산 대비 사용률
SELECT gpu_device_id,
       vram_used_mb,
       vram_budget_mb,
       round(100.0 * vram_used_mb / vram_budget_mb, 1) AS use_pct,
       evictions, reloads
  FROM pg_stat_gpu_cache;
```

**기대 출력 (정상):**
```
 gpu_device_id | vram_used_mb | vram_budget_mb | use_pct | evictions | reloads
---------------+--------------+----------------+---------+-----------+---------
             0 |         1638 |          28000 |     5.9 |         0 |       0
```
**→ `use_pct < 80%`, `evictions = 0`:** 용량 충분  
**→ `evictions > 0` 또는 `use_pct >= 90%`:** budget 부족 → `--max-vram-mb` 상향 또는
기존 인덱스 REINDEX 필요성 검토

```sql
-- 실제 빌드된 artifact 크기와 추정 비교
SELECT index_name, n_vecs, dim,
       round(vram_bytes/1048576.0) AS vram_mb_actual,
       round(n_vecs::numeric * (dim*4.0+256) / 1048576, 1) AS vram_mb_est
  FROM pg_stat_gpu_search;
-- actual ≈ est 이면 estimate_vram_bytes 공식이 적중한 것
```

```bash
# artifact 크기 실측 (1M×384 예시)
ls -lh /tmp/cuvs_indexes/ | grep -E "\.cagra|\.tids|\.delta"
# .cagra 크기는 직렬화 결과를 기록한다. .tids는 1M 기준 약 8 MiB.
```

- [ ] `pg_stat_gpu_cache.evictions = 0` (또는 허용 범위 내)
- [ ] `use_pct < 80%`
- [ ] `vram_mb_actual ≈ vram_mb_est` (추정 공식 검증)
- [ ] 빌드 완료 후 `resident = t`

> 검증 근거: `estimate_vram_bytes`(src/pg_cuvs_server.c:447) + `cuvs_auto_shard_count`
> (src/cuvs_util.c) 코드 로직; pilot 실측(crossover-methodology.md §11: 1M×384 CAGRA build
> 28s, §16: 1M×1024 CAGRA build 85s); 50M×384 한계 확인(§12: 2×A100-40GB 교착 → 단일 노드
> 한계).

---

## 6. Escalation 기준 (When to escalate)

- `final_approx > n_gpus × per_gpu_budget` (shard cap 이후에도 초과): 더 큰 GPU VM
  또는 GPU 수 증가가 필요하다. 50M×384의 특정 실측 실패만으로 다른 구성의 최소 요건은 확정되지 않는다.
- 빌드 중 데몬이 `no CUDA GPUs detected` 또는 `VRAM budget exceeded`로 죽는 경우:
  `vram-oom-fallback.md` + `gpu-vm-lifecycle.md` 참조.
- `--max-vram-mb`를 물리 VRAM 90% 이상으로 설정해도 빌드 실패 반복: CUDA 내부
  컨텍스트 + cuBLAS 임시 버퍼가 예상보다 큼 → `build_mem_safety_ratio` 하향(0.4).
- `delta.vram_bytes` 계산에서 41 MiB(1024d, 10K rows)가 VRAM budget을 압박:
  `cuvs.max_delta_rows` 축소 또는 REINDEX 주기 단축 검토.

관련: `vram-oom-fallback.md`, `multi-gpu-sharding-ops.md`, `benchmark-runbook.md`.  
설계 근거: ADR-021(샤딩), ADR-024(sharded eviction), src/pg_cuvs_server.c estimate_vram_bytes.

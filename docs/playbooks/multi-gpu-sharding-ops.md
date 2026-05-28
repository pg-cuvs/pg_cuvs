# Playbook: 멀티 GPU 샤딩 운영 (build · placement · fanout · eviction)

단일 logical CAGRA 인덱스를 N개 shard로 GPU에 분산하는 Phase 3F/3G 기능의 운영.
빌드/배치 확인, parallel fanout, over-fetch 튜닝, 손상 shard fail-closed, VRAM
압박 시 whole-unit eviction을 다룬다.

---

## 1. 증상 (Symptoms)

- sharded 인덱스를 만들었는데 `pg_stat_gpu_shards`에 행이 안 보인다.
- 쿼리가 sharded GPU path 대신 CPU로 빠진다(`cagra index not loaded ...`).
- 새 sharded build가 `shard 0 won't fit on any GPU`로 실패한다.
- `cuvs.parallel_fanout=on/off`를 바꿔도 `p50_latency_us`가 동일하게 나온다.
- 한 shard 파일이 손상된 뒤 쿼리가 ERROR거나 부분 결과가 의심된다.

---

## 2. 확인 명령 (Diagnostic commands)

```sql
-- shard 배치 (shard별 GPU/크기/오프셋/상주)
SELECT shard_id, gpu_device_id, n_vecs, tid_offset, vram_used_mb, resident, search_count
  FROM pg_stat_gpu_shards WHERE index_name='<idx>' ORDER BY shard_id;
-- logical 인덱스의 shard_count (0/NULL gpu = sharded)
SELECT index_name, shard_count, gpu_device_id, resident
  FROM pg_stat_gpu_search WHERE index_name='<idx>';
-- GPU별 VRAM/eviction/reload
SELECT gpu_device_id, resident_count, vram_used_mb, vram_budget_mb, evictions, reloads
  FROM pg_stat_gpu_cache;
```
```bash
# 디스크 artifact: .shards manifest + .sNNN.cagra (commit marker는 .shards)
ls -1 /tmp/cuvs_indexes/*_*.shards /tmp/cuvs_indexes/*_*.s0??.cagra 2>/dev/null
# 데몬 로그: auto count / 배치 / 손상
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "auto shard|built sharded|shard [0-9].*GPU|crc mismatch"
```

---

## 3. 원인 분기 (Cause branches)

### A. shard_count 의미
`cuvs.shard_count`: **0 = auto**(VRAM로 추론; 한 GPU에 들어가면 1=unsharded),
**1 = 강제 unsharded**, **N≥2 = 강제 N shard**. build 시점에만 읽는다(CREATE INDEX/
REINDEX). auto가 ≥2를 만들려면 인덱스 추정 VRAM이 per-GPU budget(`--max-vram-mb`)을
초과해야 한다(단일 GPU에서는 GPU 1개라 auto는 1로 떨어진다 — sharding은 multi-GPU에서).

### B. build가 "won't fit on any GPU"
per-GPU budget 합보다 큰데 GPU 수/`CUVS_SHARDS_MAX`/`n_vecs/2` 한도로 충분히 못
쪼갠 경우, 또는 **dead 인덱스가 budget을 점유**한 경우. 후자는 3G.1(DROP cleanup)/
3G.4(eviction)로 해소 — `drop-and-write-path-diagnosis.md` 참조.

### C. parallel_fanout 효과가 p50에 안 보임
`pg_stat_gpu_search.p50/p95`는 **log2 버킷**이라 sequential(sum)과 parallel(max)이
같은 octave면 동일 값으로 양자화된다. 정밀 비교는 `avg_latency_us`(=total/count)를
쓴다(검증된 예: SEQ avg 1492µs vs PAR avg 1053µs).

### D. shard별 recall이 낮음
shard 수가 늘면 각 shard에서 top-k만 받는 정책의 recall이 떨어질 수 있다.
`cuvs.shard_overfetch`(기본 0)를 올려 shard별 `k+slop`로 over-fetch한다.

### E. 손상/누락 shard → fail-closed
shard `.cagra`의 crc가 `.shards` manifest 기록과 어긋나면 reload 시
`shard N artifact crc mismatch ... skip`, logical 인덱스 미등록(0 shard rows),
쿼리는 부분 결과 없이 CPU fallback/ERROR. **partial 결과는 절대 반환하지 않는다.**

---

## 4. 복구 절차 (Recovery steps)

### sharded 인덱스 빌드 / 배치 확인
```sql
SET cuvs.index_dir='/tmp/cuvs_indexes';
SET cuvs.shard_count=2;   -- 또는 0(auto), N
CREATE INDEX mg_cagra ON mg USING cagra (v vector_l2_ops);
-- 배치 확인: 멀티 GPU면 shard들이 서로 다른 gpu_device_id 에 분산되어야 함
SELECT shard_id, gpu_device_id, n_vecs FROM pg_stat_gpu_shards WHERE index_name='mg_cagra' ORDER BY shard_id;
```

### parallel fanout A/B (latency)
```sql
-- 데몬 stats를 리셋하려면 데몬 restart 후 측정 (reload from .shards manifest)
SET cuvs.parallel_fanout=off;  -- N회 쿼리 후 avg 확인
SELECT round(avg_latency_us) FROM pg_stat_gpu_search WHERE index_name='mg_cagra';
-- 데몬 restart → SET cuvs.parallel_fanout=on; → N회 쿼리 후 avg 비교 (PAR < SEQ 기대)
```

### over-fetch 튜닝
```sql
SET cuvs.shard_overfetch=32;   -- recall 부족 시 상향, 기본 0 = 3F 동작
```

### 손상 shard 복구
```bash
# 데몬 로그에서 crc mismatch 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep "crc mismatch"
```
```sql
-- 정답은 REINDEX (base 재빌드 → .shards/.sNNN.cagra 새 generation으로 교체)
REINDEX INDEX mg_cagra;
```

### VRAM 압박 / eviction (3G.4)
sharded 인덱스는 **whole-unit으로 evict** 가능(모든 shard + tids + delta 캐시 해제,
artifact는 디스크에 durable하므로 save 불필요). 압박 시 LRU로 빠지며, 이후 쿼리가
`.shards` manifest로 reload한다. in-flight lock-free 검색 중인 인덱스는 evict되지
않는다(inflight refcount). 별도 조치 불필요 — 다만 budget이 만성 부족이면
`--max-vram-mb` 상향 또는 shard 수/배치를 조정한다.

---

## 5. 검증 명령 (Verification commands)

```sql
-- top-k가 CPU exact와 일치 (parallel == sequential == CPU; clustered 데이터 기준)
SET cuvs.k=10; SET enable_seqscan=off;
SET enable_cuvs=on;  SET cuvs.parallel_fanout=on;
CREATE TEMP TABLE g AS SELECT id FROM mg ORDER BY v <-> (SELECT v FROM mg WHERE id=42) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE c AS SELECT id FROM mg ORDER BY v <-> (SELECT v FROM mg WHERE id=42) LIMIT 10;
SELECT (SELECT array_agg(id ORDER BY id) FROM g) = (SELECT array_agg(id ORDER BY id) FROM c) AS topk_match;
-- 양쪽 GPU shard counter 증가 확인 (멀티 GPU)
SELECT shard_id, gpu_device_id, search_count FROM pg_stat_gpu_shards WHERE index_name='mg_cagra' ORDER BY shard_id;
```
> 주의: uniform-random 합성 데이터는 CAGRA recall이 스케일에서 무너진다(unsharded도
> 동일). recall 검증은 **clustered/실 임베딩** 데이터로 한다.

---

## 6. Escalation 기준 (When to escalate)

- auto(`shard_count=0`)가 multi-GPU에서도 1로만 떨어지면: 인덱스 추정 VRAM이
  per-GPU budget 미만이라 정상(작은 인덱스). 강제 분산은 `shard_count=N`.
- `crc mismatch`가 REINDEX 후에도 재발하면: 디스크/파일시스템 손상 의심 →
  `persistence-corruption-recovery.md`.
- parallel이 sequential보다 느리면(드묾): thread spawn 오버헤드 > GPU 이득(shard가
  너무 작음). shard 수 축소 또는 `parallel_fanout=off`.

관련: `gcs-snapshot-ops.md`, `drop-and-write-path-diagnosis.md`, `vram-oom-fallback.md`.
설계 근거: ADR-021(샤딩), ADR-022(parallel fanout safe-by-construction), ADR-024(eviction).

# Playbook: VRAM OOM 및 CPU Fallback

VRAM 고갈로 인한 build-time OOM, search-time CPU fallback, circuit breaker 동작에 대한
진단 및 복구 절차. ADR-010, ADR-011 참조.

---

## 1. 증상 (Symptoms)

**빌드 시 (CREATE INDEX):**
```
ERROR:  pg_cuvs: BUILD failed (status 2); CREATE INDEX aborted to preserve catalog durability
HINT:   GPU VRAM exhausted. Free VRAM (drop other cagra indexes or restart pg_cuvs_server) and retry, ...
```

**검색 시 (SELECT, 실행 중 실패):**
```
ERROR:  pg_cuvs: GPU VRAM exhausted; retry will use CPU while breaker is open
ERROR:  pg_cuvs: GPU daemon unavailable after planning; retry will use CPU while breaker is open
```

실행 중 GPU 호출이 OOM 또는 daemon unavailable을 반환하면 해당 SELECT는 ERROR로
끝날 수 있다. 이후 breaker가 열리면 다음 계획은 CPU 경로로 재계획된다.
실패한 SELECT가 같은 실행에서 자동으로 다른 index/seqscan으로 이어진다고 가정하지 않는다.

---

## 2. 진단

```bash
nvidia-smi --query-gpu=memory.total,memory.used,memory.free --format=csv,noheader,nounits
```

**기대 출력:**
```
40960, 8192, 32768
```
(total, used, free — 단위: MiB)  
**→ 정상:** free가 신규 인덱스 로드에 충분한 여유  
**→ 이상 시:** free가 수백 MiB 이하 → Build-time OOM이면 Step 1로, Search-time fallback이면 Step A로

---

```bash
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'loaded index|built index|evict|VRAM'
```

**기대 출력:**
```
pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)
```
**→ 정상:** 인덱스 로드 메시지 존재  
**→ 이상 시:** `VRAM budget exceeded` 또는 `evict_lru` 메시지 → VRAM 한계에 도달한 상태

---

```bash
sudo systemctl cat pg-cuvs-server | grep ExecStart
```

**기대 출력:**
```
ExecStart=/usr/local/bin/pg_cuvs_server --index-dir /tmp/cuvs_indexes --max-vram-mb 32768 ...
```
**→ 참고:** 생략 시 기본 예산은 GPU별 물리 VRAM의 90%다. 명시값은
`pg_stat_gpu_cache.vram_budget_mb`와 함께 확인한다.

---

```sql
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
SET cuvs.debug = off;
```

**기대 출력 (GPU 정상):**
```
NOTICE:  pg_cuvs: cagra scan index_oid=XXXXX gpu
```
**기대 출력 (fallback 상태):**
```
(NOTICE 없음, 결과만 반환)
```
**→ NOTICE 없음:** GPU path가 아님 → fallback 또는 circuit breaker open 상태

---

```sql
SHOW cuvs.circuit_breaker_threshold;
```

**기대 출력:**
```
 cuvs.circuit_breaker_threshold
---------------------------------
 3
(1 row)
```
**→ 정상:** 기본값 3  
**→ 참고:** circuit breaker는 process-local이므로 세션별로 상태가 다를 수 있음

---

## 3. 원인 분기 (Cause branches)

### A. Build-time OOM: ensure_vram 실패 (status=2, OOM_FALLBACK)

`handle_build`에서 `ensure_vram(needed)`가 -1을 반환한다.
LRU eviction을 수행했지만 `--max-vram-mb` 예산 또는 물리 VRAM 한계를 넘었다.

예산·eviction·거부 경로:
```
Layer 1 — admission: 최종 인덱스와 scratch 예산을 확인
Layer 2 — reclaim/eviction: 정책에 따라 다른 상주 인덱스의 VRAM 회수 시도
Layer 3 — rejection: 확보하지 못하면 OOM_FALLBACK으로 빌드 거부
```

build-time OOM은 DDL 실패로 처리된다 (query path와 달리 fallback success 없음).
→ 복구 Step 1로

### B. Search-time OOM: handle_search에서 cuvs_cagra_search 실패

`cuvs_cagra_search` 반환 != 0 시 `CUVS_STATUS_OOM_FALLBACK`을 반환한다.
실행 중 OOM이면 `gettuple`이 SQLSTATE `53200` ERROR를 낸다. 같은 SELECT는
중단되므로 트랜잭션을 정리한 뒤 다음 SELECT에서 CPU 재계획 여부를 확인한다.
→ 복구 Step A로

### C. Circuit breaker 트리거

GPU 오류가 `cuvs.circuit_breaker_threshold`(기본 3)회
연속 발생하면 해당 인덱스에 대해 circuit breaker가 열린다.
이후 해당 인덱스로의 모든 쿼리는 `WARNING` 없이 자동으로 CPU로 라우팅된다 (process-local).
→ 복구 Step C로

### D. --max-vram-mb가 너무 낮게 설정됨

물리 VRAM이 충분한데도 OOM이 발생하면 `--max-vram-mb` 값이 지나치게 보수적인 것이다.
`nvidia-smi`의 `memory.total`을 기준으로 적절한 값을 설정한다.
→ 복구 Step 3으로

---

## 4. Step-by-step 복구

### Build-time OOM 복구

---

### Step 1 — 현재 VRAM 점유량 파악

```bash
nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits
```

**기대 출력:**
```
5000, 40960
```
(free MiB, total MiB)  
**→ 성공:** 수치 확인 후 Step 2로  
**→ 실패:** nvidia-smi 명령 오류 → CUDA/드라이버 상태 확인

---

### Step 2 — 불필요한 cagra 인덱스 제거

```sql
SELECT indexname, tablename FROM pg_indexes
WHERE indexdef LIKE '%USING cagra%';
```

**기대 출력:**
```
    indexname    | tablename
-----------------+-----------
 cagra_idx_old   | items_old
 cagra_idx       | items
(2 rows)
```

불필요한 인덱스를 DROP한다 (daemon에서 해당 항목이 evict되고 VRAM 해제됨):

```sql
DROP INDEX <unused_index_name>;
```

DROP 후 VRAM 재확인:

```bash
nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits
```

**→ free 증가 확인:** Step 4로  
**→ free 여전히 부족:** Step 3으로

---

### Step 3 — --max-vram-mb 조정

```bash
sudo systemctl cat pg-cuvs-server
sudo systemctl edit pg-cuvs-server
```

**기대 출력:**
```
(편집기 열림)
```

override에서 기존 `ExecStart=`를 빈 값으로 초기화한 뒤, 원래 인수를 보존한
새 `ExecStart=` 행의 `--max-vram-mb`를 수정한다. GPU별 물리 여유와
빌드 scratch를 고려한다.
예: 물리 40960 MiB → `--max-vram-mb 32768`

저장 후:

```bash
sudo systemctl daemon-reload
sudo systemctl restart pg-cuvs-server
```

**기대 출력:**
```
(에러 없이 완료)
```
**→ 성공:** Step 4로  
**→ 실패:** journal 확인 → `sudo journalctl -u pg-cuvs-server -n 20 --no-pager`

---

### Step 4 — CREATE INDEX 재시도

```sql
CREATE INDEX cagra_idx ON items USING cagra (embedding vector_l2_ops);
```

**기대 출력:**
```
CREATE INDEX
```
**→ 성공:** 검증 체크리스트로  
**→ 실패:** `ERROR: pg_cuvs: BUILD failed (status 2)` → Step 3 반복 또는 Escalation

---

### Search-time Fallback 복구

---

### Step A — fallback 상태 확인

```sql
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
SET cuvs.debug = off;
```

실행 중 OOM이면 ERROR로 끝난다. 다음 SELECT가 결과만 반환하면
`pg_cuvs_circuit_status()`와 `EXPLAIN`으로 CPU 경로를 확인한다.
**→ ERROR:** Step B로
**→ GPU NOTICE 없음:** Step C에서 breaker와 실제 계획 확인

---

### Step B — 예산과 다른 상주 인덱스 확인

```bash
sudo systemctl status pg-cuvs-server --no-pager
sudo journalctl -u pg-cuvs-server -n 50 --no-pager
```

**기대 출력:**
```
(에러 없이 완료)
```

인덱스가 미상주이거나 데몬이 실패 상태라면 `daemon-restart-recovery.md`를
따른다. 데몬이 정상이고 budget 거부라면 다른 상주 인덱스와 물리 여유를
확인한 뒤, 원인을 조정하고 GPU path 복귀를 확인한다:

```sql
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
SET cuvs.debug = off;
```

**기대 출력:**
```
NOTICE:  pg_cuvs: cagra scan index_oid=XXXXX gpu
```
**→ 성공:** GPU path 복귀 확인 → 검증 체크리스트로  
**→ 실패:** NOTICE 여전히 없음 → Step C 확인 또는 Escalation

---

### Step C — circuit breaker 해제

circuit breaker 상태를 확인한다:

```sql
SELECT * FROM pg_cuvs_circuit_status()
 WHERE index_oid = 'cagra_idx'::regclass;
```

**기대 출력:**
```
 index_oid | state | transient | opened_at | half_open_count | consecutive_errors
-----------+-------+-----------+-----------+-----------------+-------------------
     16392 | open  | t         | ...       |               0 |                 3
```
이 함수는 호출한 backend의 breaker만 표시한다. `transient=true`인 open은
`cuvs.circuit_breaker_transient_ttl_s`가 지난 뒤 half-open으로 전환될 수 있다.

인덱스 OID를 확인한다:

```sql
SELECT oid, relname FROM pg_class WHERE relname = 'cagra_idx';
```

**기대 출력:**
```
  oid  | relname
-------+---------
 16392 | cagra_idx
(1 row)
```

circuit breaker를 해제한다:

```sql
SELECT pg_cuvs_reset_circuit('cagra_idx'::regclass);
```

**기대 출력:**
```
 pg_cuvs_reset_circuit
-----------------------

(1 row)
```
**→ 성공:** Step A로 돌아가 GPU path 복귀 확인  
**→ 함수 없음:** pg_cuvs 버전이 `pg_cuvs_reset_circuit()`을 지원하지 않음 → idle 연결을 kill하여 process-local circuit breaker 초기화

---

### 비상 조치 — GPU path 완전 비활성화

```sql
-- 이 세션에서만 CPU 강제 사용
SET enable_cuvs = off;
```

`enable_cuvs = off`는 `PGC_USERSET`이므로 세션 단위로 적용된다.
전역 적용이 필요하면 `ALTER SYSTEM SET enable_cuvs = off`를 사용하되, 이는 임시 조치다.

---

## 5. 검증 체크리스트

- [ ] `nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits` → 기대 출력: 인덱스 로드에 충분한 여유 MiB 값
- [ ] `sudo journalctl -u pg-cuvs-server --no-pager | tail -20` → 기대 출력: `evict_lru` 또는 `saved index` 메시지 없이 `loaded index` 존재
- [ ] `SET cuvs.debug = on; SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;` → 기대 출력: `NOTICE: pg_cuvs: cagra scan ... gpu`
- [ ] `SELECT * FROM pg_cuvs_circuit_status();` → 현재 backend의 breaker 상태 확인

---

## 6. Escalation 기준 (When to escalate)

- `--max-vram-mb`와 물리 여유가 충분한데도 build-time OOM이 반복되면:
  `pg_cuvs_last_rejection()`과 daemon journal에서 VRAM/호스트 RAM/물리 GPU
  preflight 사유를 구분한다. 같은 SQLSTATE만으로 원인을 단정하지 않는다.
- circuit breaker가 threshold=1 설정에서도 매 쿼리 트리거되면:
  GPU search 자체가 반복적으로 실패하는 것. daemon journal의 `OOM_FALLBACK` 이유 확인 후
  에스컬레이션.
- `pg_cuvs_reset_circuit()`이 없는 pg_cuvs 버전에서는 backend 프로세스를 종료
  (idle 연결 kill)하면 process-local circuit breaker가 초기화된다.

# Playbook: 릴리스 및 업그레이드 (release-upgrade)

pg_cuvs 설치/재설치, 데몬 재시작 순서, artifact magic 호환성 확인 절차.
현재 저장소의 `default_version`은 **0.9.3**이다.
`0.1.0`부터 `0.9.3`까지의 SQL migration chain이 저장소에 있고,
`test/sql/upgrade_path.sql`이 설치·업데이트 경로를 회귀 검증한다.

이 playbook은 현재 버전의 신규 설치/재설치, cross-version UPDATE,
코드 변경 후 재배포 절차를 다룬다. 실제 운영 전환에서는 설치된 구버전과
대상 버전에 맞는 migration script 및 백업/롤백 절차를 함께 확인한다.

---

## 1. 증상 (Symptoms)

- 소스 변경 후 `make install`만 했는데 데몬 기능(GCS snapshot, HNSW 사이드카
  직렬화 등)이 반영되지 않는다.
- 재설치 후 실행 중 검색 쿼리가 daemon 연결 오류를 내거나 후속 쿼리가 CPU로 재계획된다.
- 데몬 재시작 없이 `.so` 교체 후 기존 backend 세션이 이전 코드로 동작한다.
- 업그레이드 후 `loaded index` 메시지 없이 데몬이 뜨거나, artifact magic 오류가
  journal에 나온다.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# 설치된 확장 버전
psql -d postgres -c "SELECT name, default_version, installed_version
                     FROM pg_available_extensions WHERE name='pg_cuvs';"
```

**기대 출력:**
```
 name   | default_version | installed_version
---------+-----------------+-------------------
 pg_cuvs | 0.9.3           | 0.9.3
```
**→ `installed_version IS NULL`:** `CREATE EXTENSION pg_cuvs`가 아직 실행되지 않음  
**→ `installed_version != default_version`:** `ALTER EXTENSION pg_cuvs UPDATE` 필요
  (설치된 버전에서 현재 버전까지의 migration chain을 먼저 확인)

```bash
# 설치된 바이너리/라이브러리 타임스탬프 (재설치가 실제로 적용됐는지 확인)
ls -la $(pg_config --pkglibdir)/pg_cuvs.so
ls -la $(which pg_cuvs_server 2>/dev/null || echo /usr/local/bin/pg_cuvs_server)
```

```bash
# 데몬 현재 상태 및 바이너리 경로
sudo systemctl status pg-cuvs-server --no-pager
sudo systemctl cat pg-cuvs-server | grep ExecStart
```

```bash
# artifact magic/version (reload 실패 시 확인)
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "magic|version|artifact|tids|cagra"
```

---

## 3. 원인 분기 (Cause branches)

### A. `make install`만 실행 — 데몬 바이너리 미갱신
`make install`은 PostgreSQL 확장 `.so`(pg_cuvs.so)와 `.control`/SQL 파일만 설치한다.
`pg_cuvs_server` 데몬 바이너리는 **`make install-server`**로 별도 설치한다.
GCS 업로드/다운로드, HNSW 사이드카 직렬화(`from_cagra()`), sharded snapshot 등
모든 데몬 기능은 이 바이너리에 있다.
→ 복구 Step 1 (install + install-server).

### B. `.so` 교체 후 기존 백엔드 세션이 이전 코드 사용
PostgreSQL 백엔드는 `shared_preload_libraries`로 로드된 `.so`를 프로세스 시작 시
한 번만 dlopen한다. `make install` 후 새 쿼리 세션에도 이전 `.so`가 쓰이려면
PostgreSQL을 재시작(`pg_ctl restart`)해야 한다. 데몬은 별도 프로세스이므로 데몬도
재시작해야 새 바이너리가 적용된다.
→ 복구 Step 2 (올바른 재시작 순서).

### C. artifact magic/version 불일치
각 artifact는 magic 바이트와 version 필드를 파일 헤더에 기록한다:
- `.tids`: magic `'TIDS'` (4바이트) + version 필드
- `.cagra`/`.shards`: 자체 magic 바이트

데몬이 artifact를 reload할 때 magic/version이 현재 코드와 맞지 않으면 해당
인덱스를 건너뛴다(journal에 magic 불일치 메시지). 이 경우 REINDEX로 현재 버전
artifact를 새로 만들어야 한다.

> 현재 대상 버전은 0.9.3이다. 소스 변경 없이 같은 코드베이스로 재빌드하면
> magic은 동일하지만, format-breaking 변경이 있는 버전 전환은 해당 migration
> script와 REINDEX 필요성을 별도로 확인한다. 저장소의 전체 버전 전환 경로는
> `test/sql/upgrade_path.sql`에서 다룬다.
→ 복구 Step 3 (REINDEX로 artifact 재생성).

### D. `ALTER EXTENSION pg_cuvs UPDATE` (cross-version)
`pg_cuvs--<old>--<new>.sql` migration script가 있을 때 아래 명령으로 카탈로그를
업데이트한다. 현재 저장소에는 `pg_cuvs--0.7.0--0.8.0.sql`,
`0.8.0--0.8.1`, `0.8.1--0.8.2`, `0.8.2--0.9.0`, `0.9.0--0.9.1`, `0.9.1--0.9.2`, `0.9.2--0.9.3`을 포함한
0.1.0→0.9.3 chain이 있다.

---

## 4. 복구 절차 (Recovery steps)

### Step 1 — 재설치 (`.so` + 데몬 바이너리)

```bash
# 소스 디렉토리에서 실행
make install          # pg_cuvs.so + .control + SQL 파일
make install-server   # pg_cuvs_server 바이너리
```

**기대 출력:**
```
/bin/install ... pg_cuvs.so
/bin/install ... pg_cuvs_server
```
**→ `make install` 성공 + `make install-server` 성공:** Step 2로  
**→ `make install-server` 명령 없음:** Makefile 버전 확인(Phase 3I 이후 추가됨)

VM 원격 배포 시:
```bash
make sync              # 소스 VM 동기화
make gpu-build         # VM에서 빌드
make gpu-install       # make install 실행
make gpu-server        # pg_cuvs_server 빌드 + make install-server
make gpu-reload        # PostgreSQL과 daemon 재시작
make gpu-verify-deployed
```

---

### Step 2 — 올바른 재시작 순서

배포에는 `make gpu-reload`를 우선 사용한다. 이 타겟은 PostgreSQL을 먼저,
데몬을 다음에 재시작하고 데몬이 `active`가 될 때까지 기다린다.

```bash
# 1. PostgreSQL 재시작 (.so 교체 반영, 기존 세션 종료)
sudo systemctl restart postgresql@16-main

# 2. 데몬 재시작 (새 pg_cuvs_server 바이너리 적용)
sudo systemctl restart pg-cuvs-server

# 3. 데몬이 listening 중인지 확인
sudo journalctl -u pg-cuvs-server --no-pager -n 20 | grep "listening"
# 기대: pg_cuvs_server: listening on /tmp/.s.pg_cuvs
```

**기대 출력 (데몬 로그):**
```
[INFO] pg_cuvs_server: GPU 0 (NVIDIA A100-SXM4-40GB): 40448 MB total, budget N MB
[INFO] pg_cuvs_server: loaded index <db>/<idx> (N vecs, N MB VRAM)
[INFO] pg_cuvs_server: listening on /tmp/.s.pg_cuvs
```
**→ `loaded index` 없음:** artifact 없거나 magic 불일치 → Step 3  
**→ `no CUDA GPUs detected`:** `gpu-vm-lifecycle.md` (driver mismatch) 참조

---

### Step 3 — artifact magic 불일치 시 REINDEX

```bash
# journal에서 magic/version 불일치 인덱스 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "magic|invalid|version mismatch"
```

해당 인덱스 이름을 찾아:

```sql
-- 현재 버전 artifact로 재생성
REINDEX INDEX <idx>;
```

**기대 출력:**
```
REINDEX
```

```bash
# 재빌드 후 artifact 타임스탬프 갱신 확인
ls -lh /tmp/cuvs_indexes/ | grep -E "\.cagra|\.tids"
```

**기대:** 현재 시각으로 갱신된 파일

---

### Step 4 — cross-version extension update

```bash
# 1. 설치된 버전에서 현재 버전까지의 migration script가 있는지 확인
ls sql/pg_cuvs--*.sql

# 2. .control 파일의 default_version이 새 버전인지 확인
grep default_version pg_cuvs.control

# 3. 재설치
make install && make install-server

# 4. PostgreSQL 재시작 (새 공유 라이브러리 적용)
sudo systemctl restart postgresql@16-main

# 5. 카탈로그 업데이트 (각 데이터베이스에서 실행)
psql -d <dbname> -c "ALTER EXTENSION pg_cuvs UPDATE;"

# 6. 데몬 재시작 (새 바이너리 적용)
sudo systemctl restart pg-cuvs-server
```

저장소의 전체 설치·업데이트 경로를 CPU shim에서 재현하려면
`test/sql/upgrade_path.sql`을 실행한다. 운영 환경에서는 먼저 staging에서
동일한 `ALTER EXTENSION` 경로와 artifact reload를 검증한다.

---

## 5. 검증 명령 (Verification commands)

```bash
# 확장 버전 일치
psql -d postgres -c "SELECT installed_version FROM pg_available_extensions
                     WHERE name='pg_cuvs';"
# 기대: 0.9.3
```

```bash
# 데몬 active + loaded index
sudo systemctl is-active pg-cuvs-server
# 기대: active
sudo journalctl -u pg-cuvs-server --no-pager | grep "loaded index"
# 기대: loaded index <db>/<idx> (<N> vecs, <N> MB VRAM) — 1줄 이상
```

```sql
-- GPU 검색 경로 정상 동작
SET enable_cuvs=on; SET enable_seqscan=off;
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM <table> ORDER BY v <-> '[...]'::vector LIMIT 10;
-- 기대: "Custom Scan (CuVSScan)" 또는 NOTICE에 "cagra scan ... gpu"
```

```sql
-- pg_stat_gpu_search 에서 resident + 검색 카운터 증가
SELECT index_name, resident, search_count, last_status
  FROM pg_stat_gpu_search;
-- 기대: resident=t, last_status='ok'
```

- [ ] `installed_version = default_version = 0.9.3`
- [ ] `sudo systemctl is-active pg-cuvs-server` → `active`
- [ ] journalctl에 `loaded index` 1줄 이상
- [ ] `EXPLAIN` 결과에 GPU scan path 확인
- [ ] `pg_stat_gpu_search.resident = t`

> 검증 근거: `daemon-restart-recovery.md` (재시작 후 reload 검증, 소켓/index_dir
> 경로 동기화 확인); crossover-methodology.md §16.5 주의사항 (`make install-server`
> 별도 필요 — `make install`만으로는 `.so`만 갱신).

---

## 6. Escalation 기준 (When to escalate)

- `make install-server` 후에도 journal에서 `from_cagra()` / GCS 업로드 기능이 없는
  구버전 메시지: 데몬 바이너리 경로 확인(`which pg_cuvs_server`) — 이전 설치본이
  PATH 앞에 있을 수 있음.
- magic 불일치로 REINDEX를 해도 즉시 재발: 빌드한 `.so`와 `pg_cuvs_server`의
  소스 커밋이 다를 가능성(각각 별도 빌드됨) → 같은 소스 상태에서 `make` 전체 재빌드.
- cross-version 업그레이드 후 `ALTER EXTENSION` 실패(`could not open file pg_cuvs--<old>--<new>.sql`):
  설치된 버전에서 대상 버전까지의 migration script 누락 여부를 확인하고 release blocker로 기록한다.
- PostgreSQL 재시작 후 `shared_preload_libraries`에서 pg_cuvs.so 로드 실패:
  `pg_config --pkglibdir` 경로가 설치 경로와 맞는지 확인.

관련: `daemon-restart-recovery.md`, `persistence-corruption-recovery.md`.  
설계 근거: `pg_cuvs.control(default_version=0.9.3)`,
`test/sql/upgrade_path.sql`, Makefile `install` / `install-server` 타겟.

---

## 7. Upgrading 0.9.2 to 0.9.3 (cover guarantee)

**Status: Unverified for release; Current / Verified only for the steps tagged
"executed" below.** 0.9.3 is not released. The executed steps come from
`test/unit/test_downgrade_pg.py` (this repository's test runner, run on a relocated
PostgreSQL 16.15 cluster); the other runners for this feature are
`test/unit/test_rollback_hazards_pg.py` and `test/unit/test_cover_breakglass_pg.py`.
This section names them; it does not record a run of them at the time of writing. The
`pg_cuvs_hnsw` indexes in those tests were created by switching the access method of a
pgvector `hnsw` index in the catalog; they are not GPU-built indexes. What was not run is
listed in section 7.5 and in "Not verified" of
[rollback-and-cleanup.md](rollback-and-cleanup.md) section 6.4.

0.9.3 adds the cover guarantee for `pg_cuvs_hnsw` indexes: a cover descriptor in the
metapage and a ledger of keys on extra pages (see `CHANGELOG.md`). Its SQL moves the
access method handler to a new C symbol, `pg_cuvs_hnsw_handler_c1p`, which the 0.9.2
library does not export. That is why the order below matters.

### 7.1 Order

1. Install the package. The running server still uses the old code.
2. **Restart the server. The restart is mandatory.** Installing without restarting leaves
   the postmaster on the library image it already loaded, and every new backend keeps using
   it (executed: `test_downgrade_pg.py`, case S4).
3. In every database, run `ALTER EXTENSION pg_cuvs UPDATE;`. The extension version is per
   database, so a cluster with several databases needs one update in each.
4. Indexes built before 0.9.3 stay LEGACY: they carry no cover descriptor and have no
   guarantee. Only indexes built from 0.9.3 on carry one.

Check the result in each database:

```sql
SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs';   -- 0.9.3
SELECT pg_cuvs_hnsw_cover_status('schema.index_name'::regclass);
```

### 7.2 `shared_preload_libraries`

`pg_cuvs` must be in `shared_preload_libraries`. Without it the library cannot be loaded and
`CREATE EXTENSION pg_cuvs` fails with `FATAL: cannot create PGC_POSTMASTER variables after
startup` (executed: `test_downgrade_pg.py`, case S0). This is not a new requirement of
0.9.3; the current library already needs it.

### 7.3 `ALTER EXTENSION UPDATE` run before the restart

Executed: `test_downgrade_pg.py`, case S4.

- Symptom: `ERROR: could not find function "pg_cuvs_hnsw_handler_c1p" in file
  ".../pg_cuvs.so"`.
- Effect: none. The update script runs in one transaction, so the extension stays at 0.9.2
  with the old handler symbol, and tables stay readable and writable.
- Recovery: restart the server, then run the same `ALTER EXTENSION` again.

### 7.4 Connection pools

After the update, reconnect every pooled connection. A session opened before
`ALTER EXTENSION pg_cuvs UPDATE` keeps the old index routine in its relation cache, and a
`REINDEX` run from such a session builds the index as LEGACY again (executed in an
independent review, script `st1.sh`; it is not one of the three test runners).

### 7.5 Not run for this procedure

- Indexes that the GPU built over a table that already had rows (the test environment has
  no CAGRA daemon), and a `pg_dump` restore of such a table.
- A cluster whose databases are at different steps of this procedure.
- Platforms other than Linux with glibc, for "a replaced file is not used until the
  restart".

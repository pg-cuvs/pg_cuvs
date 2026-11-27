# Playbook: 배포 Rollback 및 Cleanup

잘못된 배포를 복구하거나 확장을 완전히 제거할 때 쓴다. 두 경로를 구분한다.
현재 저장소의 기본 확장 버전은 `0.9.3`이다.
**배포 복구**는 인덱스와 heap을 유지하며 호환되는 바이너리를 다시 배포한다.
**완전 제거**는 인덱스와 확장 카탈로그를 DROP한 뒤 대상 artifact를 정리한다.
artifact는 heap에서 재생성 가능한 파생 데이터지만, 원본 heap과 다른
데이터가 선택 범위에 없는지 확인한 뒤에만 삭제한다.

---

## 1. 증상과 경로 선택

- 설치한 `.so`와 데몬이 서로 다른 코드 상태이거나 실행 프로세스가 이전
  바이너리를 사용하는 경우 → §3 **배포 복구**.
- journal에 artifact magic/version 불일치가 있는 경우 → 호환되는 바이너리
  배포 후, 인덱스가 카탈로그에 남아 있으면 대상 인덱스만 `REINDEX`.
- 확장과 인덱스를 실제로 제거하려는 경우 → §4 **완전 제거**.

구버전 바이너리로 되돌릴 때 현재 확장 SQL 카탈로그와 IPC·artifact 형식이
호환되는지 먼저 확인한다. 이 저장소의 migration chain은 이전 버전에서
0.9.3으로 가는 경로이며, SQL 다운그레이드 명령을 제공하지 않는다.
호환성이 확인되지 않으면 바이너리만 되돌려 정상 복구됐다고 판단하지 않는다.

## 2. 현재 상태와 대상 기록

```bash
git rev-parse HEAD
pg_config --pkglibdir
pg_config --bindir
sudo systemctl cat pg-cuvs-server | grep ExecStart
sudo systemctl is-active pg-cuvs-server
sudo journalctl -u pg-cuvs-server -n 50 --no-pager
```

`pg_cuvs_server` 설치 경로는 `Makefile`의 `install-server`가
`$(pg_config --bindir)`를 사용한다. 실제 서비스가 실행하는 경로는
`systemctl cat`의 `ExecStart`로 확인한다. 두 경로가 다르면 먼저 그 차이를
해결한다.

```sql
SELECT current_database(), oid AS db_oid
  FROM pg_database WHERE datname = current_database();
SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_cuvs';
SHOW shared_preload_libraries;
SHOW config_file;
SHOW cuvs.index_dir;
SHOW cuvs.socket_path;
SELECT n.nspname, c.relname, c.oid AS index_oid, am.amname
  FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
  JOIN pg_am am ON am.oid = c.relam
 WHERE am.amname IN ('cagra', 'flat', 'ivfpq', 'pg_cuvs_hnsw')
 ORDER BY 1, 2;
```

각 대상 데이터베이스에서 실행해 DB OID, 인덱스 OID, per-index
`index_dir` reloption, extension 버전을 기록한다. 확장이 이미 없으면
`SHOW cuvs.*`가 실패할 수 있으므로 unit의 인수와 보존된 설정을 확인한다.
**기대:** 배포 복구에서는 인덱스가 카탈로그에 남아 있다. 완전 제거에서는
DROP 전에 대상 목록이 확정된다. 현행 설치의 `extversion` 기대값은 `0.9.3`이다. 구버전 rollback을 선택한 경우에는 선택한 revision의
확장 버전과 카탈로그 호환성을 별도로 확인한다.

---

## 3. 배포 복구: 인덱스 보존

### 3.1 현재 소스 상태를 다시 배포

`gpu.conf`의 `VM_SSH_HOST`와 conda 설정을 확인한다. GCP 자동 IP 조회를
쓰는 환경이면 `USE_GCP_IP=1`을 설정한다. 로컬 저장소의 의도한 소스
revision에서 다음 공식 타겟을 실행한다:

```bash
make gpu-deploy
```

`Makefile`은 `sync → gpu-build → gpu-install → gpu-server → gpu-reload →
gpu-verify-deployed`를 실행한다. `gpu-reload`는 PostgreSQL을 먼저,
데몬을 다음에 재시작하고 두 서비스가 `active`인지 확인한다.
**기대:** 명령 종료 코드 0과 신선도 검사 통과. `gpu-build` 실패에 한해
VM의 `/tmp/pg_cuvs_build.log`를 확인한다. 설치·reload·신선도 검사 실패는
해당 단계 출력과 두 서비스 journal로 진단한다. Makefile은
`build complete`라는 완료 문자열을 보장하지 않는다.

새 VM에 `.so`가 아직 설치되지 않았다면
[GPU VM 빌드·설치 절차](gpu-vm-build-and-test.md)를 따른다.
`gpu-postinstall`은 최소한 `gpu-install`로 `.so`를 설치한 뒤에 실행하는
초기 설정 단계다. 매번 배포 대신 쓰는 타겟은 아니다.

### 3.2 구버전으로 되돌려야 하는 경우

운영자가 지정한 revision의 확장 SQL 카탈로그·데몬 IPC·artifact 형식이
현재 데이터베이스와 호환되는지 확인한 뒤 해당 revision을 **별도 로컬
checkout**으로 준비한다. 그 checkout의 `gpu.conf`와 `Makefile`을 확인하고
§3.1의 `make gpu-deploy`를 실행한다. 현재 저장소에 SQL 다운그레이드
스크립트가 없으므로 `ALTER EXTENSION ... UPDATE`로 이전 버전으로
되돌린다고 안내하지 않는다.

### 3.3 artifact 불일치 또는 인덱스 미상주

```sql
SELECT index_name, resident, last_status, last_error
  FROM pg_stat_gpu_search;
```

journal과 상태에서 대상 인덱스가 확인되고 **카탈로그에 여전히 존재하면**
쓰기 가능한 노드에서 해당 인덱스만 재생성한다:

```sql
REINDEX INDEX schema.cagra_idx;
```

**기대:** `REINDEX` 출력, 이후 대상 인덱스의 artifact 갱신과 검색 경로 확인.
이미 `DROP INDEX` 또는 `DROP EXTENSION`한 인덱스에는 `REINDEX`를 실행할 수
없다. 완전 제거 뒤 다시 설치하는 경우에는 heap에서 `CREATE INDEX`를
새로 실행한다. 읽기 전용 standby에서는 `REINDEX`하지 않는다.

---

## 4. 완전 제거: 카탈로그와 대상 artifact 정리

이 경로는 해당 데이터베이스의 인덱스를 실제로 제거한다. §2에서 모든
대상 DB·인덱스 OID·경로를 기록하고, 운영자가 전체 삭제 대상을 확정한 뒤
실행한다. `vector` 확장은 다른 객체가 사용할 수 있으므로 여기서는 제거하지
않는다.

### 4.1 서비스 중지와 DROP

```bash
sudo systemctl stop pg-cuvs-server
sudo systemctl is-active pg-cuvs-server
sudo journalctl -u pg-cuvs-server -n 30 --no-pager
```

**기대:** `is-active`가 `inactive`. 정상 종료 로그에는
`pg_cuvs_server: SIGTERM received, serializing indexes...`,
`sigterm: N indexes saved, M failed`,
`pg_cuvs_server: shutdown complete`가 남는다. `failed`가 0인지 확인한다.
종료 로그가 없거나 실패가 있으면 journal과 artifact 상태를 확인한다.

각 대상 데이터베이스에서 §2에 기록한 인덱스를 DROP한다. 아래 이름은
예시이며 실제 목록으로 치환한다:

```sql
DROP INDEX schema.cagra_idx;
DROP EXTENSION pg_cuvs;
SELECT extname FROM pg_extension WHERE extname = 'pg_cuvs';
```

**기대:** DROP 명령 성공, 마지막 조회 0행. 의존 객체가 남아 있으면
DROP 오류를 읽고 대상 범위를 다시 확정한다. 인덱스가 이미 없으면
해당 DROP을 실행하지 않는다.

### 4.2 preload 설정 제거

```sql
SHOW shared_preload_libraries;
SHOW config_file;
-- 다른 preload 항목은 보존하고 pg_cuvs만 뺀 전체 목록을 지정한다.
ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements';
```

마지막 문자열은 **예시**다. 다른 항목이 없다면 `''`를 지정한다.
`SHOW config_file`이 실제 설정 파일 경로를 준다. `postgresql.conf`나
`postgresql.auto.conf`의 다른 `shared_preload_libraries` 설정이 우선해
`pg_cuvs`가 남는지 확인한다. 설정 변경 후 PostgreSQL을 재시작한다:

```bash
sudo systemctl restart postgresql@16-main
psql -d postgres -c 'SHOW shared_preload_libraries;'
```

**기대:** 출력에 `pg_cuvs`가 없다. 실제 PostgreSQL unit이 다르면 그
unit을 사용한다. 재시작 실패 시 해당 PostgreSQL journal과
`SHOW config_file`로 확인한 설정을 점검한다.

### 4.3 정확한 artifact와 stale socket 정리

데몬을 중지한 상태에서 §2에 기록한 OID와 실제 `index_dir`를 사용한다.
데몬 기본 디렉터리 밖의 per-index 경로도 각각 확인한다.

```bash
INDEX_DIR=/tmp/cuvs_indexes
DB_OID=16384
INDEX_OID=16392
sudo find "$INDEX_DIR" -maxdepth 1 -type f -name "${DB_OID}_${INDEX_OID}.*" -print
```

**기대:** 기록한 인덱스의 `.cagra`, `.tids` 등 파생 sidecar만 출력된다.
목록을 카탈로그 DROP 기록과 대조하고, 원본 데이터나 다른 소유 파일이
없음을 확인한 뒤 정확히 이 범위만 삭제한다:

```bash
sudo find "$INDEX_DIR" -maxdepth 1 -type f -name "${DB_OID}_${INDEX_OID}.*" -delete
sudo find "$INDEX_DIR" -maxdepth 1 -type f -name "${DB_OID}_${INDEX_OID}.*" -print
```

**기대:** 두 번째 `find`는 출력이 없다. 다른 인덱스 파일은 유지된다.
대상이 여러 개면 각 DB·인덱스에 대해 목록 확인과 삭제를 반복한다.

stale socket이 있으면 §2의 `SHOW cuvs.socket_path`와 서비스 `--socket`
인수가 같은 경로인지 대조하고, 데몬이 `inactive`인 상태에서만 제거한다:

```bash
SOCKET_PATH=/tmp/.s.pg_cuvs
sudo systemctl is-active pg-cuvs-server
if sudo test -S "$SOCKET_PATH"; then sudo rm -- "$SOCKET_PATH"; fi
sudo test ! -e "$SOCKET_PATH" && echo 'socket absent'
```

**기대:** 서비스 `inactive`, 마지막 줄 `socket absent`. 경로가 다르거나
서비스가 active이면 삭제하지 않는다.

---

## 5. 검증과 Escalation

**배포 복구:**

```bash
make gpu-verify-deployed
```

```sql
SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs';
SELECT index_name, resident, last_status FROM pg_stat_gpu_search;
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
```

**기대:** 배포 신선도 검사 성공, 대상 extension·인덱스 존재,
검색 계획과 결과가 의도한 경로를 사용한다. 예시 쿼리는 실제 테이블·차원에
맞게 치환한다. plan-time CPU 우회와 실행 중 ERROR는 별도로 진단한다.

**완전 제거:** 각 대상 DB에서 `pg_extension`에 `pg_cuvs`가 없고, §4.3의
정확한 `find`가 빈 결과이며, PostgreSQL preload 목록에 `pg_cuvs`가
없고 데몬이 `inactive`인지 확인한다.

- `make gpu-deploy`의 빌드·설치·reload·신선도 검사 중 어느 단계가 실패했는지
  먼저 구분한다. 설치된 경로와 서비스 `ExecStart`가 다르면 해당 unit을 확인한다.
- `REINDEX`가 반복 실패하면 대상 인덱스의 카탈로그 존재, 거부 SQLSTATE,
  `pg_cuvs_last_rejection()`, 데몬 journal을 함께 보존한다.
- `DROP EXTENSION` 실패는 의존 객체와 DB별 범위를 확인한다. `CASCADE`로
  예상 밖 객체를 제거하지 않는다.

---

## 6. Going back from 0.9.3 to 0.9.2 (cover guarantee)

Sections 1 and 3.2 above say the repository has no SQL downgrade script. That holds for
every version except this one: `sql/pg_cuvs--0.9.3--0.9.2.sql` takes 0.9.3 back to 0.9.2,
and `sql/pg_cuvs--0.9.2--0.9.3.sql` brings it forward again.

The executed claims in this section come from three test runners:
`test/unit/test_downgrade_pg.py` (cases S0 to S5), `test/unit/test_rollback_hazards_pg.py`
(R1 to R3) and `test/unit/test_cover_breakglass_pg.py` (G1 to G4), run on relocated
PostgreSQL 16.15 clusters. This document names them; it does not record a run of them at the
time of writing. Their `pg_cuvs_hnsw` indexes were created by switching the access method of
a pgvector `hnsw` index in the catalog, so they are not GPU-built indexes. Anything without an
"executed" mark is not established by those runners and is also listed in section 6.4.

### 6.1 Planned downgrade

**Status: Current / Verified for the 0.9.3 branch under test, on the runners above; 0.9.3 is
unreleased.**

Do this before the package is replaced, while the 0.9.3 library is still loaded, in each
database:

```sql
ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2';
SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs';   -- 0.9.2
```

If another session holds a temporary table with a `pg_cuvs_hnsw` index, the command stops
with an error that names that index. Run it again after that session ends. Then downgrade the
package and restart.

The script does three things (executed: `test_downgrade_pg.py`, cases S2 and S5):

1. `pg_cuvs_cover_invalidate_all()` takes the guarantee mark off every `pg_cuvs_hnsw` index.
   If any index cannot be taken off, the script stops with an error and nothing changes. Every
   backend then rebuilds its relation cache for the index.
2. The handler goes back to the old C symbol.
3. The functions that 0.9.3 added are dropped.

What happens to each index state (executed: case S5, current build):

| Index state | What the script does |
|---|---|
| LEGACY | Passes through unchanged. |
| OFF, LOST, damaged (UNTRUSTED) | Pass through unchanged; none of them is a guarantee mark. |
| CERTIFIED | Written as LOST with reason `downgrade` (`graph_epoch` + 1, crc recomputed). An UNLOGGED index is written in both the main fork and the init fork, because a crash reset copies the init fork over the main fork. A CERTIFIED index in a newer format that this library cannot write is refused, and the hint says to take the mark off with that newer library or to `DROP INDEX`. |

To go forward again, install the new package, restart, and run
`ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'` (executed: S2 round trip). An index that became
LOST has to be recertified before the guarantee holds again; the status hint names
`VACUUM` or `pg_cuvs_cover_recertify()` for that.

### 6.2 Unplanned rollback: older package, SQL still at 0.9.3

**Status: Current / Verified for the symptoms and the emergency action (R1 to R3, G1 to G4);
Unverified for the recovery by plain reinstall and for the cases in section 6.4.**

This is the state where the package was replaced with 0.9.2 but the database was not taken
back with section 6.1. It is the intended trade-off: an older library must not write silently
into an index of the new format, so it cannot open that index at all.

#### Symptoms

Executed: `test_rollback_hazards_pg.py`, R1 to R3, on indexes made with a real
`CREATE INDEX USING pg_cuvs_hnsw` (created on an empty table and filled by inserts).

On a table with a `pg_cuvs_hnsw` index, all of the following fail with `ERROR: could not find
function "pg_cuvs_hnsw_handler_c1p" in file ".../pg_cuvs.so"`:

- `SELECT` (even one that does not use the index, such as `SELECT 1 FROM t`), `INSERT`,
  `UPDATE`, `DELETE`, `VACUUM`, `VACUUM (FREEZE)`, `REINDEX`, `TRUNCATE`, `DROP INDEX`,
  `DROP TABLE` and `DROP EXTENSION pg_cuvs CASCADE`. An independent review also saw
  `DROP SCHEMA ... CASCADE` and `ALTER TABLE ... DROP COLUMN` on the vector column fail.
- **Logical backup.** A full `pg_dump` fails, because `pg_get_indexdef` opens the index.
  The dump fails if any table that has a `pg_cuvs_hnsw` index is still part of it. In an
  independent review with two such tables, `rt` and `rt2`, `-T rt` failed, `-T rt -T rt2`
  passed, `-a -T rt` failed, and `-t other` (a table without such an index) passed. So `-T`
  helps only when every table with a `pg_cuvs_hnsw` index is excluded. To get data out of a
  table that has one, use `COPY t TO STDOUT`.
- **Autovacuum keeps failing on that table.** The worker is always a new process, so it opens
  the index and gets the same error. In the run it tried every naptime (2 seconds there), failed
  each time, `last_autovacuum` stayed empty and `n_dead_tup` did not fall (3 or more attempts
  within 40 seconds). A table without such an index in the same database was vacuumed normally.
- **Anti-wraparound vacuum fails too.** The run set `autovacuum_freeze_max_age` to its minimum
  (100000) on two tables, one with the index and one without, and consumed 130,000 transactions.
  The table without the index was frozen by anti-wraparound autovacuum (its age fell to about
  0, with `vacuum_freeze_min_age = 0`). The table with the index kept an `age(relfrozenxid)` of
  130,000 or more, and each autovacuum attempt failed with the same error (2 or more extra
  attempts after the consumption). While that table is not frozen, `datfrozenxid` of the
  database cannot advance either. The run did not come near the real limit of about 2 billion
  transactions, so do not leave this state in place for long.
- What still works: tables without such an index, `ALTER TABLE ... ADD COLUMN`, and
  `COPY t TO STDOUT` (it opens neither the planner path nor the index). `COPY (SELECT ...) TO`
  fails.
- `ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'` fails with the older library too, because the
  script calls `pg_cuvs_cover_invalidate_all`, which that library does not have. After the
  error the version and the symbol are unchanged.

#### Recovery

1. **Reinstall the new package and restart the server.** This is the expected recovery. The
   older library could not open the indexes, so it wrote nothing to them, and the guarantee
   marks are not expected to have changed. No runner exercises this plain reinstall as a case
   of its own (see section 6.4).
2. If the older package has to stay, a superuser runs one library-provided function in each
   database (executed: `test_cover_breakglass_pg.py`, G1, with the older library installed):

```sql
SELECT pg_cuvs_cover_breakglass();
```

   The function is plain SQL, so it runs with either library installed. It does two things:
   (1) it points the handler back at the old C symbol, and (2) it creates the marker function
   `pg_cuvs_cover_breakglass_marker()` and makes it a member of the extension. Afterwards the
   older library opens the indexes again, autovacuum and anti-wraparound vacuum run (in the
   run, the table age fell from 130,000 to 1), and inserts work. The state is a **mismatch:
   extension version 0.9.3 with the old symbol**. Running the function twice gives the same
   state. Only a superuser can run it.

#### How the guarantee cannot become silently invalid after the emergency action

The older library records no ledger keys and its VACUUM changes the graph, so during this
period a CERTIFIED mark can stop matching reality. The protection is on the new library's side,
and the marker is what it keys on.

- **While the marker function exists, the new library never answers from a guarantee mark
  approximately.** Every CERTIFIED index reads as LOST with reason `break-glass`, whichever
  symbol the handler names. With the new symbol, the scan reads CERTIFIED as LOST and answers
  by exact heap ranking. With the old symbol (the state right after the emergency action), the
  scan wrapper consults the marker for indexes that carry a cover descriptor and answers by the
  same exact heap ranking; a LEGACY index has no descriptor and never had a guarantee, so it
  behaves as in 0.9.2. Executed: G2, a scan in the old-symbol state returns the exact heap
  answer and the "emergency rollback" notice. The status function returns
  `guarantee_in_force: false`, `breakglass_marker: true` and the way to release it
  (`pending`). VACUUM does not recertify, and `pg_cuvs_cover_recertify` refuses with
  `not_applicable`. If an operator points the handler back at the new symbol by hand, the
  stale CERTIFIED marks are still not read as valid (executed: G2).
- **The only way back is two update steps.** With the new package installed and the server
  restarted, run `ALTER EXTENSION pg_cuvs UPDATE TO '0.9.2'` (writes every CERTIFIED index as
  LOST with reason `downgrade` and sets the old handler symbol), then
  `ALTER EXTENSION pg_cuvs UPDATE TO '0.9.3'` (takes every CERTIFIED mark off once more, sets
  the new symbol, and **drops the marker**). Only this update script drops the marker.
  Afterwards the indexes are LOST, so scans answer exactly from the heap, and the guarantee
  holds again only after recertification (`VACUUM` or `pg_cuvs_cover_recertify`). Executed:
  G3; after recertification `guarantee_in_force` was `true` and scans merged again.
- **Lifetime of the marker** (executed: G4). `pg_dump` does not carry the marker, because it is
  an extension member, so a restored database has none; the restored indexes were rebuilt, so
  that is correct. `pg_upgrade` (same version) carries the marker together with the index
  files, so the indexes in the upgraded cluster stay outside the guarantee. The extension
  version and the marker are per database, so the emergency action and the way back have to be
  done in each database. A cluster whose databases are at different steps was not run.
- **Lock note.** Recertification and VACUUM hold `ShareUpdateExclusiveLock` on the table while
  they walk it. If a DDL such as `ALTER TABLE` waits behind them, PostgreSQL's lock queue makes
  later reads and writes wait for as long. A manual recertification gives way when another
  session starts to wait for a lock that conflicts with its table lock (status `deferred`,
  nothing written; `test_cover_walk_pg.py` W4), and does not take its locks while a statement
  that conflicts holds them. A VACUUM does not give way: it holds the lock for its own whole run.
  Run a manual recertification of a large index when no DDL is pending.
- **Do not** run `CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler ...` by hand instead of the
  function. It leaves no marker. While the older library is in use that is harmless, but when
  the handler is later pointed back at the new symbol, stale CERTIFIED marks are read as valid
  again. The new library cannot detect this: the resulting catalog equals the normal state.
  This remaining path is closed by documentation only; if the function was used, it is closed.

### 6.3 Checks

```sql
SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs';
SELECT proname FROM pg_proc WHERE proname = 'pg_cuvs_cover_breakglass_marker';
SELECT pg_cuvs_hnsw_cover_status('schema.index_name'::regclass);
```

A marker row means the database is in the emergency state of section 6.2 and has to go back
through the two update steps.

### 6.4 Not verified

These were not run, or are inferred; none of them should be read as a tested statement above.

- GPU-built indexes (indexes built on a table that already had rows): the environment had no
  CAGRA daemon. The tests used an index created on an empty table and filled by inserts. A
  `pg_dump` restore of a table with rows (the restore rebuilds the index) was not run either.
- Causes of an unplanned rollback, such as an automatic package downgrade by the package
  manager or the promotion of a standby that has the older package: inferred from the case
  table of the working draft, not run. Standby promotion was not run.
- A cluster whose databases are at different extension versions or marker states: inferred,
  not run. (`pg_dump` restore and `pg_upgrade` were run, G4.)
- The recovery by reinstalling the new package alone: expected, but not an executed case of its
  own. The emergency path and the round trip through two updates were run (G1 to G4).
- A real exhaustion of 2 billion transaction IDs. The failure of anti-wraparound vacuum was
  shown with `autovacuum_freeze_max_age` at its minimum, not at the real limit.
- A long-lived backend that keeps inserting during a downgrade (the relation cache is cleared
  by `pg_cuvs_cover_invalidate_all`, but a statement already running in a connected session is
  a separate case).
- The lock-yield behaviour of recertification (section 6.2) under a pending DDL.
- Linux with glibc is the only platform considered for "a replaced file is not used until the
  restart".

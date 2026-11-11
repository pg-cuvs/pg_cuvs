# Playbook: 배포 Rollback 및 Cleanup

잘못된 배포를 복구하거나 확장을 완전히 제거할 때 쓴다. 두 경로를 구분한다.
현재 저장소의 기본 확장 버전은 `0.9.0`이다.
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
0.9.0으로 가는 경로이며, SQL 다운그레이드 명령을 제공하지 않는다.
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
DROP 전에 대상 목록이 확정된다. 현행 설치의 `extversion` 기대값은 `0.9.0`이다. 구버전 rollback을 선택한 경우에는 선택한 revision의
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

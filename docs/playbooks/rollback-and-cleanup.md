# Playbook: Rollback 및 Cleanup

잘못된 배포를 되돌리거나, 확장을 제거하거나, artifact를 안전하게 정리하는 절차.

index artifact(`.cagra`/`.tids`)는 derived data다 — WAL 대상이 아니고 원본 heap 데이터가
PostgreSQL에 존재하므로 언제든 `REINDEX`로 재생성할 수 있다. 삭제해도 데이터 손실 없음.

---

## 1. 증상 (Symptoms)

- 새 버전 배포 후 pg_cuvs.so 로드 실패, IPC 프로토콜 불일치, 또는 CREATE INDEX 동작 이상.
- 구버전으로 되돌려야 하는 상황.
- 개발/테스트 환경을 완전히 초기화해야 하는 상황.
- `cuvs.index_dir` 경로를 변경하거나 artifact를 다른 위치로 이동해야 하는 상황.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# 현재 설치된 .so 버전 확인
ls -la $(pg_config --pkglibdir)/pg_cuvs.so
ls -la $(pg_config --bindir)/pg_cuvs_server

# 현재 extension 버전
psql -d postgres -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs';"

# artifact 목록 및 크기
ls -lh /tmp/cuvs_indexes/
# cuvs.index_dir 경로가 다르면:
psql -d postgres -c "SHOW cuvs.index_dir;"

# daemon 상태
sudo systemctl is-active pg-cuvs-server

# shared_preload_libraries 확인 (rollback 후 제거 필요 여부)
psql -d postgres -c "SHOW shared_preload_libraries;"
```

---

## 3. 원인 분기 (Cause branches)

### A. .so와 pg_cuvs_server 버전 불일치
IPC 프레임 구조(CuvsCmdFrame, CuvsReplyHeader)가 바뀐 경우 구버전 daemon과 신버전 .so,
또는 그 반대 조합에서 통신 오류가 발생한다.
증상: `ERROR: pg_cuvs: BUILD failed (status 1)` 또는 검색 응답이 쓰레기값.

### B. 신버전 .so가 구버전 artifact를 읽지 못함
`.tids` 포맷 변경(magic/version 변경) 시 구버전 artifact는 `validation failed`로 skip된다.
-> 모든 인덱스 REINDEX 필요.

### C. postgresql.conf에 잘못된 설정이 남음
rollback 후 `shared_preload_libraries`에 pg_cuvs가 남아 있으면 postmaster 재시작 시
구버전 또는 제거된 .so를 로드하려다 실패할 수 있다.

---

## 4. 복구 절차 (Recovery steps)

### 4-1. daemon 중지

```bash
# SIGTERM으로 정상 종료 — 메모리 상주 인덱스를 disk에 serialize
sudo systemctl stop pg-cuvs-server

# 완료 확인
sudo journalctl -u pg-cuvs-server --no-pager | tail -5
# "shutdown complete" 또는 "sigterm: N indexes saved" 확인
```

### 4-2. extension 제거 (PostgreSQL catalog에서)

```sql
-- 모든 cagra 인덱스 DROP (extension DROP 전 선행 필요)
-- pg_indexes에서 목록 확인
SELECT indexname, tablename FROM pg_indexes WHERE indexdef LIKE '%USING cagra%';

DROP INDEX cagra_idx;       -- 인덱스마다 반복
-- 또는 테이블 기준
DROP INDEX CONCURRENTLY cagra_idx;

-- extension 제거
DROP EXTENSION pg_cuvs;
DROP EXTENSION vector;      -- pg_cuvs가 vector 타입을 사용하는 경우만
```

### 4-3. artifact 정리

artifact는 derived data이므로 안전하게 삭제할 수 있다.

```bash
# index_dir의 artifact 전체 제거
rm -rf /tmp/cuvs_indexes/
# 또는 선택적 제거
rm /tmp/cuvs_indexes/<db_oid>_<index_oid>.cagra
rm /tmp/cuvs_indexes/<db_oid>_<index_oid>.tids
rm -f /tmp/cuvs_indexes/*.tmp

# 소켓 파일 제거
rm -f /tmp/.s.pg_cuvs
```

artifact를 삭제하지 않고 다른 경로로 이동하는 경우:

```bash
# 이동 (인덱스 재생성 없이 경로만 변경)
sudo systemctl stop pg-cuvs-server
mv /tmp/cuvs_indexes /var/lib/postgresql/16/main/cuvs_indexes
# systemd unit의 --index-dir 와 postgresql.conf의 cuvs.index_dir 를 새 경로로 수정
sudo systemctl start pg-cuvs-server
```

### 4-4. .so 및 바이너리 교체 (이전 버전으로)

```bash
# 이전 버전 소스로 체크아웃 후 VM에서 빌드 + 설치
make sync
make gpu-build
make gpu-install   # sudo make install
make gpu-server    # 바이너리도 교체
```

### 4-5. shared_preload_libraries에서 제거 (extension 완전 제거 시)

```sql
-- pg_cuvs를 shared_preload_libraries에서 제거
ALTER SYSTEM SET shared_preload_libraries = '';
-- 또는 다른 preload 항목이 있으면 해당 항목만 남기고 제거
```

```bash
# PostgreSQL 재시작
sudo systemctl restart postgresql
psql -d postgres -c "SHOW shared_preload_libraries;"
# pg_cuvs가 없어야 함
```

### 4-6. 재배포 및 재인덱싱

새 버전을 배포하고 extension을 재설치한 뒤 인덱스를 재생성한다.

```bash
make sync
make gpu-build
make gpu-install
make gpu-server
make gpu-postinstall   # shared_preload_libraries 재설정 포함
```

```sql
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION pg_cuvs;

-- 인덱스 재생성 (heap 데이터는 그대로)
CREATE INDEX cagra_idx ON items USING cagra (embedding vector_l2_ops);
```

---

## 5. 검증 명령 (Verification commands)

```bash
# extension이 제거되었는지 확인
psql -d postgres -c "SELECT extname FROM pg_extension WHERE extname = 'pg_cuvs';"
# 0 rows 여야 함

# artifact가 제거되었는지 확인
ls /tmp/cuvs_indexes/ 2>/dev/null || echo "directory gone or empty"

# 재설치 후 smoke test
psql -d postgres -c "SELECT amname FROM pg_am WHERE amname = 'cagra';"
# cagra 출력

sudo systemctl is-active pg-cuvs-server
# active
```

```sql
-- 재인덱싱 후 검색 동작 확인
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
-- NOTICE: pg_cuvs: cagra scan ... 확인
SET cuvs.debug = off;
```

---

## 6. Escalation 기준 (When to escalate)

- `DROP EXTENSION pg_cuvs`가 `ERROR: extension "pg_cuvs" does not exist`가 아닌
  다른 오류로 실패하면: `pg_depend`에 잔여 의존이 있는 것. `DROP INDEX`로 의존 객체를
  먼저 제거한 뒤 재시도.
- `shared_preload_libraries` 변경 후 postmaster 재시작이 `FATAL: could not load library`로
  실패하면: 제거하지 못한 `.so` 참조가 남아 있는 것. postgresql.conf 또는
  `pg_ctl show`로 설정 파일 경로를 확인하고 직접 편집한다.
- artifact 삭제 후 `REINDEX`가 `ERROR: pg_cuvs: BUILD failed (status 4)`로 실패하면:
  daemon이 아직 기동되지 않은 것. daemon을 먼저 시작하고 재시도.

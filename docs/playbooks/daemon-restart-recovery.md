# Playbook: 데몬 재시작 및 인덱스 복구

`pg_cuvs_server`를 재시작한 뒤 persisted `.cagra`/`.tids` 쌍이 자동으로
reload되고 heap rebuild 없이 검색이 정상 동작하는지 확인한다.

---

## 1. 증상 (Symptoms)

- `systemctl restart pg-cuvs-server` 후 검색 쿼리가 `WARNING: pg_cuvs_server unreachable`
  또는 CPU fallback으로 빠진다.
- 재시작 후 journal에 `loaded index` 메시지가 없다.
- 재시작 후 첫 검색이 기대값을 반환하지 않는다.
- daemon이 비정상 종료(OOM killer, SIGKILL)된 뒤 소켓 파일이 남아 있다.

---

## 2. 진단

```bash
sudo systemctl status pg-cuvs-server
```

**기대 출력:**
```
● pg-cuvs-server.service - pg_cuvs GPU index server
   Loaded: loaded (...)
   Active: active (running) since ...
```
**→ 정상:** Active: active (running)  
**→ 이상 시:** `failed` 또는 `activating` → journalctl로 원인 확인

---

```bash
sudo journalctl -u pg-cuvs-server -n 50 --no-pager
```

**기대 출력:**
```
pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)
```
**→ 정상:** `loaded index` 메시지 존재 → Step 3 (검증)으로  
**→ 이상 시:** `loaded index` 없음 → 아래 분기 확인

---

```bash
ls -la /tmp/.s.pg_cuvs
```

**기대 출력:**
```
srwxrwxrwx 1 postgres postgres 0 ... /tmp/.s.pg_cuvs
```
**→ 정상:** 소켓 파일 존재하고 서비스가 running  
**→ 이상 시:** 파일 존재하는데 서비스가 `failed` → stale 소켓 → 원인 C로

---

```bash
nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits
```

**기대 출력:**
```
5000, 40960
```
**→ 정상:** free가 인덱스 로드에 충분한 여유  
**→ 이상 시:** free가 매우 낮음 → 원인 B로

---

## 3. 원인 분기 (Cause branches)

### A. .cagra/.tids 파일이 없거나 쌍이 불완전함
`startup_load_indexes`는 `.cagra` 파일을 기준으로 스캔하고 대응하는 `.tids`를 함께 검증한다.
어느 한쪽이 없으면 해당 인덱스를 건너뛴다.
-> persistence-corruption-recovery.md 참조.

### B. VRAM 부족으로 load 스킵
journal에 `VRAM budget exceeded loading` 또는 `insufficient VRAM loading` 메시지가 있다.
-> vram-oom-fallback.md 참조.

### C. stale 소켓 파일로 bind 실패
이전 daemon이 SIGKILL로 죽어 소켓을 정리하지 못한 경우.
systemd unit이 `ExecStartPre`로 소켓을 제거하지 않으면 `bind: address already in use`.
daemon 코드는 `main()` 진입 시 `unlink(g_socket_path)`를 수행하므로 정상 종료 경로에서는
발생하지 않는다. SIGKILL 후 수동 정리가 필요할 수 있다.
→ 복구 Step 2A로

### D. index_dir 경로 불일치
`postgresql.conf`의 `cuvs.index_dir`와 daemon의 `--index-dir` 인수가 다르면
PG는 다른 경로를 가리키고 daemon은 다른 경로에서 파일을 찾는다.
→ 복구 Step 2B로

### E. 소켓 권한 문제
systemd unit의 `ExecStartPost`에서 `chmod 666 /tmp/.s.pg_cuvs`가 실행되기 전에
PG backend가 connect를 시도하면 `EACCES`로 `UNAVAILABLE`이 된다.
`sleep 1`이 포함되어 있으나 부하가 높으면 race가 발생할 수 있다.

---

## 4. Step-by-step 복구

### Step 1 — 표준 재시작

```bash
sudo systemctl restart pg-cuvs-server
```

**기대 출력:**
```
(에러 없이 즉시 완료)
```
**→ 성공:** Step 2로  
**→ 실패:** `Job for pg-cuvs-server.service failed` → journalctl 확인

---

### Step 2 — journal에서 loaded index 확인

```bash
sudo journalctl -u pg-cuvs-server -n 50 --no-pager
```

**기대 출력:**
```
pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)
```
**→ 성공:** `loaded index` 있음 → Step 3 (검증)으로  
**→ 실패:** `loaded index` 없고 stale socket 의심 → Step 2A로  
**→ 실패:** `loaded index` 없고 index_dir 불일치 의심 → Step 2B로  
**→ 실패:** `loaded index` 없고 journal에 VRAM 관련 메시지 → Step 2C로

---

### Step 2A — stale 소켓 수동 정리 후 시작

```bash
ls /tmp/.s.pg_cuvs
```

**기대 출력:**
```
/tmp/.s.pg_cuvs
```

파일이 존재하면:

```bash
sudo rm -f /tmp/.s.pg_cuvs
sudo systemctl start pg-cuvs-server
```

**기대 출력:**
```
(에러 없이 완료)
```
**→ 성공:** Step 2로 돌아가 journal 재확인  
**→ 실패:** 다른 오류 메시지 → Step 2B 또는 journalctl 전체 확인

---

### Step 2B — index_dir 경로 동기화 확인

```bash
sudo systemctl cat pg-cuvs-server | grep ExecStart
```

**기대 출력:**
```
ExecStart=/usr/local/bin/pg_cuvs_server --index-dir /tmp/cuvs_indexes ...
```

```bash
psql -d postgres -c "SHOW cuvs.index_dir;"
```

**기대 출력:**
```
 cuvs.index_dir
----------------
 /tmp/cuvs_indexes
(1 row)
```
**→ 두 값이 일치:** 경로가 원인이 아님 → 다른 분기 확인  
**→ 두 값이 불일치:** systemd unit의 `--index-dir` 또는 `postgresql.conf`의 `cuvs.index_dir`를 일치시킨 뒤 해당 서비스 재시작 → Step 2로 돌아가 journal 재확인

---

### Step 2C — VRAM 부족 확인

```bash
sudo journalctl -u pg-cuvs-server --no-pager | grep -i 'VRAM\|skip'
```

**기대 출력:**
```
pg_cuvs_server: VRAM budget exceeded loading index ..., skip
```
**→ VRAM 관련 메시지 있음:** vram-oom-fallback.md 참조  
**→ 없음:** 다른 원인 → journalctl 전체 확인 후 Escalation 기준 참조

---

### Step 3 — 검증

```sql
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
SET cuvs.debug = off;
```

**기대 출력:**
```
NOTICE:  pg_cuvs: cagra scan index_oid=XXXXX gpu
 id
----
  1
(1 row)
```
**→ 성공:** NOTICE에 `cagra scan ... gpu` 포함 → 복구 완료  
**→ 실패:** NOTICE 없거나 fallback 메시지 → Step 2 재확인

---

## 5. 검증 체크리스트

- [ ] `sudo systemctl is-active pg-cuvs-server` → 기대 출력: `active`
- [ ] `sudo journalctl -u pg-cuvs-server --no-pager | grep 'loaded index'` → 기대 출력: `pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)` 1줄 이상
- [ ] `SET cuvs.debug = on; SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;` → 기대 출력: `NOTICE: pg_cuvs: cagra scan index_oid=XXXXX gpu`
- [ ] 이전과 동일한 결과 반환 (heap rebuild 없이 기존 cagra index 사용)

---

## 6. Escalation 기준 (When to escalate)

- 재시작 후 journal에 `loaded index` 메시지가 전혀 없고 `.cagra` 파일도 존재하면:
  persistence-corruption-recovery.md로 이동.
- daemon이 시작 직후 exit하면 (`systemctl is-active` = failed):
  journal 전체를 `sudo journalctl -u pg-cuvs-server --no-pager`로 확인.
  `socket`/`bind`/`CUDA` 오류 여부 확인 후 에스컬레이션.
- 재시작 10회 이상 자동 재시작(`Restart=on-failure`)이 반복되면:
  systemd restart loop를 `sudo systemctl stop pg-cuvs-server`로 멈추고
  원인을 journal에서 분석한 뒤 수동으로 재시작한다.

---

## 7. Security / hardening

#87/#119 finding — shm handoff는 daemon과 PG backend가 다른 uid로 돌아가는
전제하에 SO_PEERCRED(`shm_check_peer_owner`, 소켓 접속 시점의 owner 검증)와
CSPRNG 세그먼트 이름 + `O_EXCL`(#87)로 unlink-and-resquat 공격을 막는다.
같은 방식으로 `cuvs.daemon_uid`(#119)는 backend가 `cuvs.socket_path`에
connect하는 경로를 두 단계로 방어한다: (1) connect *전* lstat으로 소켓 파일
owner를 미리 확인해 빠른 실패 + 명확한 에러 메시지를 주고, (2) connect
*직후* SO_PEERCRED로 커널이 검증한 peer uid를 다시 확인한다. 실제 보안
경계는 (2)다 — (1)은 lstat과 connect() 사이에 소켓이 unlink·재squat될 수
있는 TOCTOU race이고, (2)는 이 특정 connection을 accept한 프로세스의
uid가 커널에 의해 확정되므로 이후 바꿔치기가 불가능하다. 기존
`shm_check_daemon_owner`(cuvs_ipc.c)는 이 (2)와 다른 검사다 —
"reply shm 소유자 == 이 소켓의 peer" 자기일관성만 보고 `cuvs.daemon_uid`와는
대조하지 않으므로, 공격자가 peer 본인이면(즉 소켓 자체를 장악했으면) 통과한다.

배포 시 다음을 지킨다:

1. **daemon을 전용 서비스 계정으로 실행한다.** "daemon과 backend가 같은 uid면
   #87/#119 방어가 의미 없다"는 daemon-vs-backend 자기자신 간 SO_PEERCRED
   검사(`shm_check_peer_owner`, #87)에만 해당하는 얘기다 — **소켓-owner
   검사(`cuvs.daemon_uid`, #119)는 다르다**: daemon uid == postgres uid인
   배포에서도, 이 검사는 호스트의 *다른 모든 로컬 uid*가 소켓 경로를
   선점하는 것을 막으므로 same-uid 배포에서도 설정할 가치가 있다.
   다만 dev VM처럼 daemon이 **대화형 로그인 유저**(SSH로 접속하는 그
   계정)로 돈다면, 그 uid는 모든 SSH 세션이 공유하는 값이라 우연히든
   의도적이든 다른 프로세스가 같은 uid로 실행되기 쉽다 — 프로덕션은
   `nobody`/`daemon` 같은 공유 계정이 아니라 데몬 전용의 좁은 서비스
   계정을 쓴다.
2. **`/dev/shm`을 sticky(모드 1777)로 유지한다.** 코드가 시작 시
   확인해 sticky가 아니면 `LOG_WARN`을 남긴다(`pg_cuvs_server.c` 데몬 시작
   경로, `startup, /dev/shm sticky check` 참고). SO_PEERCRED가 이미
   resquat된 세그먼트를 걸러내지만, sticky는 애초에 다른 로컬 사용자가
   같은 이름의 파일을 건드리지 못하게 막는 defense-in-depth다.
3. **다른-uid 또는 공유 호스트 배포에서는 `cuvs.daemon_uid`를 daemon의
   uid로 설정한다.** 예: `SET cuvs.daemon_uid = 999;` 또는
   `postgresql.conf`에 `cuvs.daemon_uid = 999`. 기본값 `-1`은 검사를
   끄며(하위 호환), 소켓 owner가 기대와 다르면 backend는 connect를
   거부하고 `CUVS_STATUS_UNAVAILABLE`로 CPU fallback한다(fail-closed).
   § 3. GUC reference의 `cuvs.daemon_uid` 참고.
4. **소켓을 non-world-writable 디렉터리에 둔다 — 이것이 1차 완화책이다.**
   기본 경로 `/tmp/.s.pg_cuvs`는 `/tmp`가 sticky(`+t`) + world-writable인
   표준 배포에서 단순히 무방비인 정도가 아니라, **sticky bit가 daemon의
   자가복구를 막아서 공격을 영구화시킨다**. 정확한 시퀀스:
   1. daemon이 재시작·크래시·부팅 순서 등으로 잠시 죽어 있는 동안,
      같은 호스트의 아무 로컬 사용자나 `/tmp/.s.pg_cuvs` 경로를 먼저
      만들고 bind해 선점한다 (sticky bit는 *새 이름으로 파일을 만드는
      것*은 막지 않는다 — 남의 기존 파일을 unlink/rename하는 것만
      막는다).
   2. daemon이 재시작을 시도하면, 자기 자신의 stale-socket 정리용
      `unlink()`(`pg_cuvs_server.c:8185`, systemd unit의
      `ExecStartPre=-/bin/rm -f /tmp/.s.pg_cuvs` — `infra/brev/bootstrap.sh:254`)가
      **sticky bit 때문에 EPERM으로 실패한다** — 그 파일은 이제 공격자
      소유이고, sticky 디렉터리에서는 파일 소유자만 자기 파일을
      지울 수 있기 때문이다.
   3. `unlink()` 실패로 stale 파일이 남은 채 `bind()`를 시도하면
      `EADDRINUSE`(`pg_cuvs_server.c:8192`)로 실패하고, daemon은
      종료된다.
   4. 공격자가 그 경로를 영구 점유하고, 이후 모든 backend가 공격자의
      리스너에 connect한다.

   즉 `cuvs.daemon_uid`가 막으려는 hijack이 **stock `/tmp` 설정에서
   실제로 도달 가능**할 뿐 아니라, sticky bit는 이 시나리오에서 방어가
   아니라 daemon의 자가복구를 방해하는 요인이다. uid 검사 유무와
   무관하게 이 공격 자체를 없애는 **1차 완화책**은 소켓을
   non-world-writable 디렉터리(예: `/run/pg_cuvs/`, root 소유 또는
   daemon 소유, 0755/0750)에 두고 `cuvs.socket_path`를 그 경로로
   설정하는 것이다. `cuvs.daemon_uid`(위 3번)는 그 위에 얹는
   defense-in-depth다.

   소켓 파일 자체의 권한 비트는 다른 얘기이자 별도로 흔히 오해되는
   지점이다: **daemon 자신은 소켓을 0660으로 만든다**
   (`pg_cuvs_server.c:8198` `chmod(g_socket_path, 0660)`). AF_UNIX
   `connect()`가 요구하는 것은 소켓 inode에 대한 **write 권한**이지
   *world*-write가 아니다 — daemon과 backend가 다른 uid인 배포에서
   올바른 방법은 backend의 OS 유저를 daemon의 그룹에 추가하는 것이다
   (`usermod -aG <daemon-group> postgres` 등). 이 repo의 dev
   bootstrap(`infra/brev/bootstrap.sh`, `infra/scripts/setup/`,
   `infra/scripts/tests/`)이 소켓을 0666으로 넓히는 것은 그 그룹
   멤버십 설정을 하지 않기 때문의 **편의**일 뿐이다(`infra/brev/README.md`
   참고) — **프로덕션에 그대로 가져가지 말 것**. `cuvs.daemon_uid`는
   world-writable 비트 자체를 거부하지 않는다 — 소켓을 unlink 후
   재squat한 공격자는 자기 uid로 새 소켓을 만들 수밖에 없으므로
   owner-uid + SO_PEERCRED 검사(위 3번)가 이미 이를 막는다.

   함정: `cuvs.socket_path`가 실제 소켓을 가리키는 **심링크**인 배포는
   `cuvs.daemon_uid` 설정 시 깨진다 — pre-check는 `lstat`으로 심링크
   자체(다른 owner일 수 있음)를 보고 거부할 수 있다. `connect()`는
   심링크를 따라가므로 실제 보안 경계인 post-connect SO_PEERCRED는
   영향받지 않지만, `cuvs.socket_path`는 심링크가 아닌 소켓 파일을
   직접 가리키게 하는 편이 안전하다.

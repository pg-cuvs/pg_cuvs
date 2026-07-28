# quality-loop 발견 패턴 축적 로그

실행할 때마다 이 파일을 먼저 읽고, 종료 시 신규 패턴을 추가한다.
형식: `- [YYYY-MM-DD] <패턴 요약> — <발견 위치> — <탐지 방법> (재발 N회)`

## 버그 클래스

- [2026-07-28] **shim이 실제 wrapper보다 "친절"해서 호출자 결함을 가린다** — `src/cuvs_wrapper_shim_cpu.c` vs `src/cuvs_wrapper.cu` — 두 파일이 같은 `cuvs_wrapper.h`를 구현하므로 함수별로 (1) 인자 무시 여부 (2) 출력 버퍼 tail 처리 (3) 반환 부호/정렬 규약을 1:1 대조. 이번 패스의 최상위 3건이 전부 이 축이었고 **전부 Tier-1 CI 100% 통과 + Tier-2에서만 발현**. 다음 패스에서도 최우선으로 볼 것.
- [2026-07-28] **한 API의 변종(variant) 간 분기 누락** — `cuvs_wrapper.cu:635` `cuvs_bf_search_filtered`가 `cuvs_bf_search`(574행)에 있는 `impl->precision` 분기를 빠뜨려 float16 인덱스에서 `*nullptr` — 같은 파일 안에서 이름이 유사한 함수쌍(`X` / `X_filtered` / `X_batch`)을 나란히 놓고 가드·분기 목록을 diff. 데몬은 단일 프로세스가 전 백엔드를 서빙하므로 SIGSEGV 하나가 전 세션을 죽인다.
- [2026-07-28] **출력 버퍼 tail 계약 불일치 (미초기화 힙 읽기)** — `pg_cuvs_server.c:2933` `praw = malloc(k)` + wrapper가 `top_k`를 코퍼스 크기로 clamp 후 `[n, k)` 미기록 — 호출자가 `for (i=0;i<k;i++)`로 순회하는데 wrapper가 k개 전부를 채우는지 확인. 쓰레기값이 우연히 유효 범위면 **실재하는 TID가 반환돼 백엔드가 못 거른다**. 주의: 0으로 초기화하면 안 된다(0은 유효 id) — 센티넬 -1이어야 한다.
- [2026-07-28] **base 설치 스크립트만 갱신하고 upgrade 스크립트를 방치** — `sql/pg_cuvs--0.2.0--0.3.0.sql` — `CREATE FUNCTION|VIEW` 이름을 base와 업그레이드 체인에서 각각 뽑아 `comm -23`. 함수 6개 + 뷰 1개가 업그레이드 사용자에게 영원히 누락됐고, 후속 `0.4.0--0.5.0.sql`의 `REVOKE`가 그 함수를 참조해 **`ALTER EXTENSION UPDATE`가 통째로 롤백**. 테스트가 전부 `CREATE EXTENSION`(base) 경로만 타서 37개 회귀 스위트가 전혀 못 잡음.
- [2026-07-28] **Makefile 수동 헤더 prereq가 실제 `#include`와 드리프트** — `Makefile:131,136` (+ `_test` 변종) — 각 `.o` 규칙의 prereq 목록과 소스의 `#include "..."`를 대조. `cuvs_util.h`가 누락됐는데 이 헤더는 로깅 매크로가 아니라 온디스크/와이어 구조체 10개(`CuvsDeltaHeader` 등)를 담고 있어 **stale object가 옛 레이아웃으로 조용히 링크**된다. 증상이 "GPU에서만 재현"처럼 보여 오진하기 쉽다.
- [2026-07-28] **소스 파일에 비인쇄 제어문자 혼입** — `sql/pg_cuvs--0.2.0.sql:3` — `\echo`의 `\e`가 ESC 바이트(0x1B)로 치환돼 있었음. `grep -rlP '\x1b'`로 전체 스캔. 격리 PG 클러스터로 재현 확인: 대조군은 `CREATE EXTENSION` 성공, ESC 버전은 `syntax error at or near ""`.

## 테스트/속도 안티패턴

- [2026-07-28] **`DROP EXTENSION`이 만드는 테스트 간 순서 결합** — `test/sql/` 37개 중 15개가 `DROP EXTENSION pg_cuvs`로 끝남 — 그 결과 정확히 15개 `.out`이 `NOTICE: extension "pg_cuvs" already exists, skipping`를 기대값으로 박아둠. **REGRESS 순서를 바꾸거나 테스트 하나를 빼면 무관한 테스트의 diff가 깨진다.** 스위트 정리를 시도하기 전에 반드시 먼저 해소해야 할 선행 항목.
- [2026-07-28] **업그레이드 경로 커버리지 0** — `test/`, `.github/`, `infra/` 전체에 `ALTER EXTENSION`/`UPDATE TO` 문자열이 단 1건도 없음 — `grep -r "ALTER EXTENSION" test/`로 즉시 확인 가능. 37개 회귀 테스트가 전부 base 스크립트만 검증.
- [2026-07-28] **부트스트랩 중복** — `build_lock.sql` ↔ `vram_accounting.sql`(8000x64 생성기 표현식까지 동일), `auto_compact.sql` ↔ `extend_vram_fallback.sql` — 파일별 `generate_series` 행수 x 차원을 표로 뽑아 정렬. 단언만 다르고 준비 과정이 같으면 한 테이블 위에서 순차 검증 가능.
- [2026-07-28] **CI 캐시 전무** — `.github/workflows/*.yml`에 `actions/cache` 0건 — PGDG repo 추가 + PG16 설치가 2개 job에서 매 push마다 cold. ccache 도입이 가장 효과 큼(단 ASAN/non-ASAN 캐시 키 분리 필요).
- [2026-07-28] **정의됐으나 테스트가 건드리지 않는 GUC 18/36개** — `auto_compact*`, `max_stale_fraction` 등 — 테스트가 참조하는 `cuvs.*`를 `src/*.c`의 `DefineCustom*` 정의와 대조. 축소가 아니라 **추가** 대상(커버리지 공백).

- [2026-07-28] **Makefile 재귀 확장 `=` + `$(shell)` = 참조 횟수만큼 프로세스 fork** — `Makefile:280` `VM_IP` — `make -n <타깃>`(아무것도 실행 안 함) 시간을 참조 0회/1회/6회 타깃끼리 비교하면 선형 증가로 즉시 드러난다. 실측 `gpu-test-all` 6.69s vs 로컬 타깃 0.24s. **더 중요한 건 정확성** — 조회 결과가 사용자 지정 호스트보다 우선하면 프로바이더 이전 후 옛 머신을 조용히 향한다(`sync`는 `rsync --delete`).
- [2026-07-28] **프로바이더 이전이 문서에만 반영되고 빌드 시스템에 미반영** — `infra/README.md`는 "Brev is the main provider now"인데 Makefile에 brev 문자열이 0건 — 이전 시 `grep -c <새프로바이더> Makefile`로 확인. 문서/스킬만 갱신하고 Makefile을 두면 런타임 오작동 경로가 남는다.

- [2026-07-28] **같은 결함 클래스를 한 곳만 고치고 형제 호출부를 방치** — `pg_cuvs_server.c:2946`(패스1 수정) vs `:6753` IVF-PQ(패스2에서야 수정) — 결함을 고친 뒤 **반드시 같은 패턴을 리포 전체에서 grep** 하라. tail 미기록 건은 소비 루프가 `break`인지 `continue`인지에 따라 심각도가 다르다(`continue`가 더 나쁨 — 첫 센티넬에서 안 멈추고 전 슬롯을 훑음).
- [2026-07-28] **수정이 문서화된 계약을 어기는 것** — `cuvs_wrapper.cu:660`에 패스1이 넣은 `return 3` — 헤더(`cuvs_wrapper.h:114`)는 0/1/2만 문서화. **가드를 추가할 때 새 반환값을 발명하지 말고 헤더 계약을 먼저 읽어라.** shim이 그 값을 낼 수 없으면 고치려던 발산을 다른 형태로 재생산하는 셈이다.
- [2026-07-28] **`.cu`가 의도적으로 비워둔 필드를 다른 함수가 읽음** — `cuvs_wrapper.cu:1679` `extract_adjacency`가 `dataset.extent(1)`를 읽는데 `deserialize`(1210)/`extend`(1283)가 그 dataset을 0x0 placeholder로 교체 — "의도적으로 비움" 주석이 달린 멤버를 grep해 **읽는 쪽 전부**를 확인하라. 이 건은 은폐가 이중이다: shim이 가릴 뿐 아니라 GPU에서도 **데몬 재시작을 끼우지 않은** 수동 테스트는 통과한다.

## VM 검증 운영 함정 (2026-07-28 세션에서 실제로 밟은 것)

- [2026-07-28] **베이스라인 없이 테스트 실패를 귀속하지 마라** — 수정본에서 34/38 실패를 보고 인덱스 오염·index_dir 불일치를 차례로 의심했으나 둘 다 오진이었다. 수정 전 트리로 스위트를 한 번 돌리자(37/37 통과) 31건 전부가 expected 노후화임이 즉시 판명됐다. **VM에 올라간 직후, 코드를 고치기 전에 베이스라인을 찍어라.**
- [2026-07-28] **`pgrep -f` / `pkill -f`는 자기 명령을 매칭한다** — `pgrep -f pg_cuvs_server`가 그 문자열을 포함한 내 ssh 명령을 잡아 죽은 데몬을 "실행중"으로 보고했고, `pkill -f "pg_cuvs_server --socket"`은 자기 셸을 죽여 뒤따르는 정리 명령이 실행되지 않았다. `pgrep -x`/`pkill -x`(정확한 이름)와 `ss -xlp`(실제 리스닝)를 쓸 것.
- [2026-07-28] **재빌드했으면 실제로 재기동했는지 확인하라** — 같은 부류로 세 번 당했다. (1) `make gpu-install`은 확장만 설치하고 데몬은 `make gpu-server`가 따로 한다. (2) `shared_preload_libraries`로 로드된 `.so`는 **PostgreSQL 재시작** 전까지 교체되지 않는다 — 이걸 놓쳐 `fallback_stat` 하나가 계속 실패했고 코드 회귀로 오인할 뻔했다. (3) ssh에서 `&`로 띄운 데몬은 세션 종료 시 죽는다(`setsid ... < /dev/null & disown` 필요).
- [2026-07-28] **회귀 스위트는 `cuvs.index_dir = '/tmp/cuvs_indexes'`를 전제한다** (35개 파일) — 그런데 `infra/brev/bootstrap.sh`는 데몬을 `/var/lib/pg_cuvs_indexes`로 띄우고 GUC도 그렇게 설정한다. 부트스트랩 직후 상태로는 스위트가 통과하지 않으므로, 데몬을 `--index-dir /tmp/cuvs_indexes`로 재기동해야 한다.
- [2026-07-28] **오탐 경고 지적 전에 베이스라인 경고 수를 세라** — 에이전트가 새 경고 5건을 만들었다고 지적했으나 HEAD와 수정본이 똑같이 62건이었다. 라인 번호만 이동한 것이었고, "인접 코드를 개선하지 마라"는 내 지시에 내 요청이 어긋났다.

## 오탐 기록 (수정하면 안 되는 것)

의도된 패턴인데 버그처럼 보이는 것들. 재발 방지용.

- [2026-07-28] **로컬 macOS `make test-unit`의 memfd/shm-grow/reaper skip** — `test/unit/test_build_corpus.c:160,257` — macOS에 `memfd_create`가 없어 걸어둔 의도적 `#ifdef __linux__` 게이트(주석에 명시). 버그가 아니라 **로컬 신호가 CI보다 약하다는 한계**. 로컬 통과를 Linux 통과로 착각하지 말 것.
- [2026-07-28] **`cuvs_ipc_export_adjacency`의 응답 헤더 필드 용도 변경** — `cuvs_ipc.c:1139-1141` (`latency_us`→graph_degree, `delta_merged`→dim, `error`→shm key) — 관례 이탈이지만 양측 해석이 일치하고 유효성 검사도 있어 버그 아님. 단, 헤더 변경 시 조용히 깨질 지점이라 주의 대상.
- [2026-07-28] **`routing_golden` ↔ `routing_golden_measured`는 중복 아님** — 전자는 `enable_*` 토글로 강제된 라우팅(티어 이식 가능), 후자는 토글 없는 비용 크기 기반 결정(측정 계수 의존). 케이스 4도 서로 다름. **통합하지 말 것.**
- [2026-07-28] **macOS에서 `-D_POSIX_C_SOURCE=200809L`로 `pg_cuvs_server.c` 문법 검사 시 `MAP_ANONYMOUS` undeclared** — 플랫폼 아티팩트이지 코드 결함이 아님(HEAD에서도 동일 재현). 로컬 검사에는 `-D_DARWIN_C_SOURCE`를 쓸 것.

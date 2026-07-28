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

## 오탐 기록 (수정하면 안 되는 것)

의도된 패턴인데 버그처럼 보이는 것들. 재발 방지용.

- [2026-07-28] **로컬 macOS `make test-unit`의 memfd/shm-grow/reaper skip** — `test/unit/test_build_corpus.c:160,257` — macOS에 `memfd_create`가 없어 걸어둔 의도적 `#ifdef __linux__` 게이트(주석에 명시). 버그가 아니라 **로컬 신호가 CI보다 약하다는 한계**. 로컬 통과를 Linux 통과로 착각하지 말 것.
- [2026-07-28] **`cuvs_ipc_export_adjacency`의 응답 헤더 필드 용도 변경** — `cuvs_ipc.c:1139-1141` (`latency_us`→graph_degree, `delta_merged`→dim, `error`→shm key) — 관례 이탈이지만 양측 해석이 일치하고 유효성 검사도 있어 버그 아님. 단, 헤더 변경 시 조용히 깨질 지점이라 주의 대상.
- [2026-07-28] **`routing_golden` ↔ `routing_golden_measured`는 중복 아님** — 전자는 `enable_*` 토글로 강제된 라우팅(티어 이식 가능), 후자는 토글 없는 비용 크기 기반 결정(측정 계수 의존). 케이스 4도 서로 다름. **통합하지 말 것.**
- [2026-07-28] **macOS에서 `-D_POSIX_C_SOURCE=200809L`로 `pg_cuvs_server.c` 문법 검사 시 `MAP_ANONYMOUS` undeclared** — 플랫폼 아티팩트이지 코드 결함이 아님(HEAD에서도 동일 재현). 로컬 검사에는 `-D_DARWIN_C_SOURCE`를 쓸 것.

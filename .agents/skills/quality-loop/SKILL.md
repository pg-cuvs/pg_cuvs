---
name: quality-loop
description: >
  pg_cuvs 코드베이스 품질 루프. 토큰 여유가 있을 때 코드 전체를 N회 훑으며
  미묘한 버그·코드 중복을 찾아 수정하고, 테스트/린트 속도를 점검·개선한다.
  발견 패턴은 PATTERNS.md에 축적해 스킬 자체가 매 실행마다 개선된다.
  "quality loop", "quality-loop", "품질 루프", "테스트 속도 점검",
  "테스트 감사", "코드 훑어줘", "중복 찾아줘" 키워드가 나오면 이 스킬을 사용하라.
---

# quality-loop — pg_cuvs 품질 루프

목적: 안정성 장치(테스트·검증)를 늘리면서도 루프 회전 속도를 지키는 것.
커버리지가 높을수록 과감한 시도가 가능하고, 테스트가 빠를수록 루프가 빨리 돈다.

## 실행 전 필수

1. 이 디렉토리의 `PATTERNS.md`를 읽는다 — 과거 발견 패턴이 이번 탐색의 우선 체크리스트다.
2. `git status`가 깨끗한지 확인한다. 진행 중 작업이 있으면 사용자에게 알리고 중단한다.
3. 반복 횟수 N을 정한다 (기본 2회, 사용자가 지정하면 그 값).

## 리포 테스트 지형 (탐색 대상)

| 계층 | 명령 | 실행 환경 | 소요 특성 |
|------|------|-----------|-----------|
| 유닛 | `make test-unit` | 로컬 Mac / CI | 초 단위, CUDA 불필요 |
| Tier-1 | `make installcheck-tier1` (PGCUVS_CPU_SHIM=1) | CI / shim 데몬 | REGRESS 전체 − TIER2_ONLY, 순차 |
| Tier-2 | `make gpu-test-*` / `make installcheck` | Brev A100 VM | GPU + 데몬 필요, 가장 느림 |
| 격리 | `make installcheck-isolation` | VM (데몬 필요) | pg_isolation_regress |

로컬 Mac에서는 CUDA 빌드가 불가하다. C 변경의 로컬 검증은
`gcc -fsyntax-only`(ci.yml Job 1 방식) + `make test-unit`까지만 가능하고,
REGRESS/격리 테스트에 영향이 가는 변경은 "VM 검증 필요"로 표시해 보고한다.

## 루프 구조 (패스당)

각 패스는 아래 4개 스코프를 순회한다. 스코프별로 explore/architect 서브에이전트에
위임해 메인 컨텍스트를 아끼되, 수정은 executor에 위임한다 (CLAUDE.md 위임 규칙).

### 스코프 A — C/CUDA 소스 (`src/*.c`, `src/*.cu`, `src/*.h`)

미묘한 버그 헌팅:
- 리소스 누수: palloc/pfree 짝, shm/fd 정리 경로, 에러 경로에서의 해제 누락
- IPC 프로토콜 드리프트: `cuvs_ipc.h` 구조체와 서버/클라이언트 양쪽 해석 불일치
- 시그널/인터럽트 안전성: CHECK_FOR_INTERRUPTS 누락, EINTR 재시도 누락
- 정수 오버플로: 벡터 수 × dim × sizeof(float) 계산부
- shim(`cuvs_wrapper_shim_cpu.c`)과 실 wrapper(`cuvs_wrapper.cu`)의 의미 불일치
  (Tier-1이 통과해도 Tier-2에서 깨지는 원인)

코드 중복: 동일 로직이 서버/확장 양쪽에 복붙된 경우, util로 추출 가능한 패턴.

### 스코프 B — 테스트 스위트 (`test/sql/`, `test/specs/`, `test/unit/`)

게시물의 7대 안티패턴을 이 리포 형태로 점검:
1. **죽은 테스트**: 이미 제거된 GUC/함수/reloption을 검증하는 테스트가 남아있는가
2. **중복 커버리지**: 두 REGRESS 테스트가 같은 경로를 또 도는가
   (예: smoke가 이미 커버하는 것을 edge_cases가 재검증)
3. **동일 부트스트래핑 반복**: 각 .sql이 같은 테이블 생성 + 인덱스 빌드를
   반복하는가 → 공용 setup 또는 작은 데이터셋으로 축소 가능한가
4. **과대 데이터셋**: 회귀 검증에 1000행이면 충분한데 10만 행을 넣는 테스트
5. **Tier 배치 오류**: GPU 없이 검증 가능한 테스트가 REGRESS_TIER2_ONLY에
   있거나, 그 반대
6. **캐시 미활용**: CI(ci.yml)에서 apt/빌드 캐시가 없어 매번 cold start인가
7. **순차 실행 병목**: pg_regress는 순차다 — 가장 느린 테스트 상위권을
   식별하고 축소 가능성을 본다 (test/results/ 타이밍 또는 CI 로그 활용)

### 스코프 C — 빌드/CI (`Makefile`, `.github/workflows/*.yml`)

- REGRESS 목록과 test/sql/ 실재 파일의 불일치 (고아 테스트, 등록 누락)
- CFLAGS에 -Wall/-Wextra 상당이 걸려 있는가; 경고를 새 결정론적 가드로
  승격할 수 있는가
- 변경 범위 대비 과도한 재빌드: 헤더 의존성 때문에 소수 수정이 전체
  재컴파일을 유발하는 지점
- gpu-* 타깃의 SSH 왕복 낭비 (한 타깃이 여러 번 접속하는 패턴)

### 스코프 D — SQL/문서 정합 (`sql/`, `pg_cuvs.control`, `design/`)

- 업그레이드 스크립트 체인과 control 파일 버전 정합
- phase-record.md의 "완료" 항목 중 실제 테스트 증거가 없는 것

## 수정 규칙 (프로젝트 가이드라인 준수)

- **Tidy First**: 구조 변경(중복 추출, 테스트 정리)과 행동 변경(버그 수정)을
  절대 같은 커밋에 섞지 않는다.
- 버그 수정은 재현 테스트 먼저 (Red → Green). 테스트 삭제/축소는 해당
  커버리지가 다른 테스트에 있음을 증거로 제시한 뒤에만.
- 수정 단위마다 검증: `make test-unit` + 영향 소스 `gcc -fsyntax-only`.
- 확신이 없는 발견은 수정하지 말고 보고서에 "제안"으로만 남긴다.
- expected/*.out 은 손으로 고치지 않는다 — 출력이 바뀌는 수정은 VM에서
  재생성해야 하므로 "VM 검증 필요"로 분류한다.

## 패스 종료 시 (자기개선 — 필수)

새로 발견한 버그 클래스·안티패턴을 `PATTERNS.md`에 1줄씩 추가한다:
`- [YYYY-MM-DD] <패턴 요약> — <발견 위치> — <탐지 방법>`
이미 있는 패턴이 재발했으면 재발 횟수를 갱신한다. 이 축적이 다음 실행의
탐색 우선순위가 된다.

## 최종 보고

- 수정 완료 목록 (커밋 단위, 구조/행동 구분 명시)
- 로컬 검증 증거 (test-unit 출력 등)
- "VM 검증 필요" 목록 (`make gpu-test-*` 대상)
- 수정하지 않은 제안 목록
- PATTERNS.md에 추가된 신규 패턴

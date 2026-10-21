# 세 번째·마지막 창: 실제 엔트리의 로컬 선행 검증

과업 `b40ecef03a934801`. 앱 source pin은 `0ff9f7c`이며, 새 생산 의미 변경은 없다.
이 디렉터리는 이전 두 실패 창을 덮어쓰지 않는다.

## 재개 전 실제 전체 경로

`run_local_fixture.py`가 **실제 upper runner의 `measurement_launch_argv`**를 호출했다.
Docker exec로 SSH transport를 대응시켜 `measurement_launch.py` → 실제 최종
`measurement_entry.py` → 고정 source의 `PgEngine` → 실제 PostgreSQL/CPU shim을
끝까지 실행했다. connect/prepare/measure 모두 exit0이고, 전체 measure 경로가
metrics/IDs/distances/metadata 저장까지 완료됐다. connect 전용 모드의 반환을 전체
측정 통과로 세지 않았다. 접속·SQL·status를 mock으로 대체하지 않았다.

실제 PostgreSQL 로그에는 검색 SQL32회와 `SHOW cuvs.rmm_pool_mb`3회가 있다.
각 점의 측정 검색은8회씩이며, 추가 검색은 warmup이다. `SELECT 1`, 실제 status와
effective PGHOST/cwd/user는 `local-validation/entry-results/connect-zero/connect.json`과
PostgreSQL statement 원문에 기록돼 있다.

| 항목 | native 설정 | 로컬 대응 |
|---|---|---|
| upper/launcher/entry byte | 최종 실행 파일 | 동일 SHA, 선행 조건에서 재검산 |
| repo/source | `/home/ubuntu/pg_cuvs`, pin0ff | 실제 shallow Git checkout, 동일 pin |
| Python 경로 | `/opt/miniforge3/envs/cuvs_bench/bin/python` | 동일 경로의 실제 venv |
| PGHOST/cwd/DB | `/var/run/postgresql`, `/home/ubuntu`, `index_accel_smoke` | 동일 |
| PostgreSQL | 16.15, x86_64, native GPU | 16.15, aarch64/musl, CPU shim |
| Python/DB user | 3.12, ubuntu | 3.14.8, postgres |
| 데이터/점 | wiki1M, k64/128, 각10K query | 실제 합성256×768 corpus, k64/128, 각8 query |
| baseline/quality | 원 graph·GT·고정 control span | 작은 fixture에는 native control 비교를 적용하지 않음 |

CPU shim의 `gpu_cagra`/query counter는 같은 protocol의 에뮬레이션 label이다.
이 로컬 결과는 **GPU 품질·성능 증거가 아니다**. native argv는 원 query 수와
기본값0의 frozen recall/p50 gate를 유지한다. `single` 검색 루프와 graph/ID 비교 AST가
원 엔트리와 같다는 보조 검산도 남겼으며 실제 SQL 실행 증거를 대신하지 않는다.

## 실제 마지막 창의 실행 제어

`local-validation/precondition.json`에는 최종 실행 입력10파일 SHA, 실제 upper argv,
native/fixture argv 차이, 실제 접속과 SQL/status 횟수가 있다. runner는 이를 확인한 뒤에만
재개한다. 전체1800초, 단계180/260/635/90/480초, 정리90+STOP480초 예약은 유지한다.
실제 VM에서는 같은 launcher/entry의 명시적 connect 모드로 SELECT1+SHOW를 먼저 확인하며
실패하면 정상 STOP으로 전환한다. 실제 measure 모드는 전체 경로를 실행한다.

이번은 세 번째이자 마지막 재개1회다. 네 번째 재개나 새 자원은 없다. 성공하면 원
native gate와 PR을 연결한다. 마지막 live창이 실패하면 native 검증을 보류로 기록한
Draft PR을 열며, CPU/shim 동작 검증과 CUDA 부분 증거를 구분한다.

## 마지막 실제 창 결과: native gate failed

실행 입력 커밋 `6967772`, 앱 pin `0ff9f7c`. 실제 VM의 SELECT1+SHOW가 통과했고,
운영과 로컬의 최종 entry SHA `6eff974166148109e87f883f0f39bb1a86cd2868e2f37e65b083df34cfe57704`가 같다.
launcher/실제 PgEngine byte도 로컬 선행 검증과 일치한다. CUDA build/co-deploy,
preflight와 RMM0의 실제 로드 byte 결속이 통과했다.

L40S(sm89), driver565.57.01, wiki1M의 기존 gd32/igd128/ivf_pq graph,
private serial SQL id+distance, supplied top10 GT, 각점10K/N=1의 결과다.
기존 고정 restart control N=3과 비교했다.

| k | 현재 recall | recall gate | 현재 p50 ms | control mean ms | 허용 absolute delta ms | p50 gate |
|---|---:|---|---:|---:|---:|---|
| 64 | 0.940680 | PASS | 1.008429 | 0.968332 | 0.003571 | FAIL |
| 128 | 0.978390 | PASS | 1.046343 | 0.999547 | 0.005279 | FAIL |

측정 GPU query는 각10,000, error0, mode gpu_cagra였다. recall은 기준 안이지만
p50은 모두 범위 밖이어서 원 gate가 실패했다. 원 threshold를 넓히거나 retry하지 않았다.
이 관측은 source나 환경에 대한 원인 규명이 아니다. 현 GPU UUID 등 실제 inventory는
`results/remote/gpu.csv`에 있으며 owner metadata의 과거 provisioning inventory와 구분한다.

default512×50과 quota 검사는 실행하지 않았다. **완료된 동작 검증 범위는 CPU/shim**이며,
이 native 부분 결과를 pool-on/성능/전체 native PASS로 표시하지 않는다. RMM live 검증은
보류이고 Draft PR로 닫는다. 그리드 fallback은 main 담당의 `fded8ce`, pool512/max미설정이며
이 pin0ff/quota50 후보를 fallback으로 사용한다고 주장하지 않는다.

### 종료와 보존

재개69.511초, 준비64.293초, 실패까지검증62.178초, 정리27.186초, STOP30.692초였다.
실패 단계는 다음 cleanup 시작 monotonic 값으로 구간을 산출했다. 전체 **253.861초**로
모든 상한 안이다. 정상 STOP 접수와 별개로 공식 `STOPPED`를
**2026-10-21 02:16:01.841620Z**에 확인했다. 원 graph와 지속 snapshot SHA는 전후 같고,
PG/daemon inactive·Result=success·ExecMainStatus0, GPU process0, old override 복원을 확인했다.
참조요금으로 계산한 약$0.1227은 실제 청구액이나 provider 종료 보장이 아니다.

이번 재개1회, 이전 두 실패 창과 합계3회이며 네 번째 재개는0회다. 새 VM·보호 Crusoe
SSH·다른 레인 접근·reset/delete도0회다. `results/`의 첫 오류·실제 metrics/IDs/distances,
source pin과 binary binding·정리·종료 원문을 보존했다. 타 VM이 들어 있는 inventory는
대상 row만 공개하고 전체 원문은 private `.omc`에 남겼다. 변환 및 원문 SHA는
`results/private-export-map.json`, 공개 파일 무결성은 `SHA256SUMS.json`에 기록했다.

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

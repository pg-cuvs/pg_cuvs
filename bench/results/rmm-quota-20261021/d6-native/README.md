# D6 실제 CUDA 후속 창 — 부분 통과, 스모크 연결 실패

과업 `980c948eac3a4d77`의 결과는 **failed**다. 실행 입력 커밋은 `1022996`,
애플리케이션 source pin은 `0ff9f7c`다. 앱 tree 일치와 원격 checkout pin을 검산했다.
원 900초 실패 창과 이번 1800초 창은 별도 기록이다.

## 실행 전 경계 검증

`deadline_budget.py`를 실제 runner에서 사용했다. 전체 1800초와 재개180/준비260/
검증635/정리90/STOP480초를 단조 증가 clock의 누적 deadline으로 묶었다.
새 검증 명령은 정리90초+STOP480초의 예약을 침범하지 못한다. 재개·별칭 갱신과
inventory/STOP 조회의 timeout도 해당 단계 잔여시간으로 자른다. 고정20회 조회는 없다.
작은 로컬 경계9종은 `deadline-checks.json`에 모두 통과로 보존했다.

실제 SSH probe는 한 번만 실행했고, 정상 hostname stdout과 warning stderr를 분리해
수용했다. 준비 단계에서 원 graph와 지속 snapshot의 SHA를 확인했다.

## 부분 통과 범위

- L40S 48GB(sm89), driver565.57.01에서 pin0ff의 확장·daemon CUDA 빌드와 동시 배포 통과.
- 배포 신선도 preflight 통과. 실제 실행 daemon SHA가 빌드 산출물과 같고, backend의
  extension maps dev/inode가 설치 파일과 일치하며 해당 파일 SHA도 빌드본과 같다.
- RMM0에서 GUC0, applied pool0/percent0/empty devices, 세 read-only 설정의 SET 거부 확인.
  원래 index가 GPU에 적재된 상태였다. nonzero pool의 상태 검증으로 승격하지 않는다.
- 원 graph와 지속 snapshot의 준비/정리 SHA가 모두 같다. 서비스 둘 다
  inactive/Result=success/ExecMainStatus=0, 정리 후 GPU process0을 확인했다.

바이너리 SHA:

| 산출물 | SHA256 |
|---|---|
| pg_cuvs.so | `ad3b83838769cbd3188e3ae229efe33561659279c5255b7a208d1038da427372` |
| pg_cuvs_server | `b4b75a99a8d64af5d5c22c96d0cb47212fa70d80ef00ed3bd2ccb57d0c903568` |

## 첫 오류와 미실행 범위

스모크 실행의 runner가 `PGHOST`를 전달하지 않았다. `PgEngine`의 첫 접속이
`/tmp/.s.PGSQL.5432`를 사용해 실패했다. 서버는 실행 중이었고, 바로 앞의 status 검사는
명시한 `/var/run/postgresql`로 성공했다. 오류 원문·traceback은 `results/state.json`과
`results/measure-zero.stderr`에 있다.

측정 검색 query는 **0회**다. RMM0의 k64/128 스모크, default512×50 arm, quota 거부는
미실행이다. 새 검색 CSV와 recall/latency 수치는 없다. 원 품질 gate는 낮추지 않았고
RMM PR도 생성하지 않았다. 이전 prototype이나 CPU/shim 결과는 이 gate를 대체하지 않는다.

현재 `run_d6.py`에는 실패 뒤 로컬에서 수정한 `env PGHOST=/var/run/postgresql` 접두부가
포함돼 있다. `check_pg_host.py`는 실제 argv 접두부로 로컬 자식 프로세스에 환경이
전달되는 것을 확인한다. 접두부 두 항목을 제외한 runner AST는 실행본1022996과 완전히
같아서 query 수·품질 gate·deadline은 바뀌지 않았다. 수정본은 원격에 배포하거나 재실행하지
않았다. `pg-host-local-check.json`이 실행본/수정본 SHA와 이 한계를 기록한다.

## 정리와 종료

| 단계 | 실제 시간 | 상한 |
|---|---:|---:|
| 재개·접속 준비 | 78.446초 | 180초 |
| 준비·빌드·배포 | 71.563초 | 260초 |
| 검증 중 실패까지 | 10.217초 | 635초 |
| 정리·export | 26.393초 | 90초 |
| 정상 STOP 요청·공식 STOPPED 관측 | 21.844초 | 480초 |

실패한 검증 단계의 시간은 다음 cleanup 단계의 시작값을 사용한 단조 clock 구간이다.
나머지 단계는 실행 중 기록한 시작/종료값이다. 전체 창은 **208.464초**였다.
정상 STOP 요청은 접수됐고 공식 `STOPPED`는 **2026-10-21 01:37:51.547046Z**에 확인했다.
요청 timeout은 provider 종료시간이나 청구시간 보장이 아니다.

보존된 $1.74/h를 전체 관측 창에 적용한 참조 계산은 약 **$0.1008**이며 실제 청구액이 아니다.
이번 재개1회, 이전 실패 창과 합계2회다. 세 번째 재개·새 VM·대체 자원·보호 Crusoe
SSH·강제 reset/delete는 모두0회다. 남은 시간을 다른 재개에 사용하지 않았다.

`results/remote/`에 build/install, source tree/pin, byte binding, graph SHA,
서비스·GPU 정리 원문을 보존했다. 다른 VM row가 포함된 Brev inventory의 전체 원문은
private `.omc`에 유지하고 공개 증거에는 대상 row만 담았다. 변환 및 원문 SHA는
`results/private-export-map.json`, 공개 파일 SHA는 `SHA256SUMS.json`에서 확인한다.

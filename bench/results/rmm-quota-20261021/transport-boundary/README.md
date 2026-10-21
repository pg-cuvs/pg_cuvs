# 로컬 SSH 소비 경계 검증과 후속 시간 제안

대상 과업: `b31cb7ac71f247f5`. 이번 단위의 원격 접속·Brev 호출·VM 재개는 0회다.
마지막 공식 VM 상태는 2026-10-21 00:49:34.419418Z의 `STOPPED`다.
애플리케이션 source pin은 `0ff9f7c`이며 production CUDA 검증은 여전히 미실행이다.

## 실제 소비 경로의 재생 결과

현장 `.omc/rmm-cap-implementation/run_native_followup.py`가 이 디렉터리의
`probe_hostname`과 `accept_probe`를 직접 호출하도록 연결했다. 같은 함수를 세 개의
로컬 Python 자식 프로세스로 실행했다. 별도 테스트 프레임워크는 사용하지 않았다.
`consumer-binding.json`에 소비자·실제 runner snapshot의 SHA와 연결 근거를 기록했다.

| 입력 | 결과 | 보존한 근거 |
|---|---|---|
| 정상 hostname stdout + warning stderr, exit0 | 수용 | 두 채널의 원래 바이트, warning 내용 |
| 다른 hostname stdout + 동일 warning stderr, exit0 | 예외로 거부 | 기대값·실제 stdout·stderr |
| SSH DNS 실패, exit255 | 수용하지 않음 | exit255·원 stderr·구체적인 오류 메시지 |

첫 시도의 `ready-12.log`는 **이미 합쳐진 로그**다. 분리된 원 stdout/stderr를
관측했다고 주장하지 않는다. 정상 hostname 접미부를 stdout, 앞의 SSH 경고를 stderr로
재구성해 입력했다. 실패 사례도 원 exit255 DNS 오류 로그를 stderr로 재구성했다.
이 구분과 원문 SHA는 `replay-results.json`에 포함된다. 재생된 두 채널의 파일 바이트는
입력과 일치하며, JSON 문자열의 개행은 Python text 읽기 규칙으로 정규화된다.

실행 명령:

```sh
python3 bench/results/rmm-quota-20261021/transport-boundary/replay_readiness.py
python3 bench/results/rmm-quota-20261021/transport-boundary/estimate_followup.py
```

runner는 다른 hostname을 즉시 거부한다. SSH 명령 자체가 실패한 경우 각 시도의
returncode와 stderr를 기록하며, 준비 시간 소진 오류에도 마지막 구체 원인을 포함한다.
로컬 HEAD에 증거 커밋만 추가된 경우에는 pin과 `bench/results` 외 tree가 같은지도
검산한다. 실제 원격 빌드 대상 pin은 바뀌지 않는다.

## 관측값

- 재개 요청 시작 → 정상 hostname 수신: **80.447초**, N=1. SSH 별칭 갱신을 포함한다.
- 중지 요청 시작 → 공식 `STOPPED` 관측: **383.676초**, N=1. CLI와 조회 대기를 포함한다.
  물리적 전원 차단 시각이나 실제 청구시간을 측정한 값은 아니다.
- 과거 같은 L40S 레인의 성공한 CUDA 전체 배포 job: **37–39초**. 이전 source의
  전체 job 시간이며 순수 compiler 시간이나 현재 pin의 측정값은 아니다.
- 과거 두 점 스모크 job: arm당 **50–54초**. 기존 graph 보존·복사·hash job: **9초**.

관련 원 시작/완료/exit 파일과 계산은 `time-plan.json`에서 역추적할 수 있다.
CUDA pin0ff의 빌드·배포, 새 status/binary binding/quota 검사와 새 정리·export,
중지 지연 분포·꼬리 상한·실제 청구시간은 **미측정**이다.

## 후속 시간 산정 — 실행 예산 제안

기존 두 arm의 k64/128 × 각10,000 query, GUC0/RMM0/default512×50,
readonly status·quota 거부·실제 binary/source/graph 결속 기준을 유지한다.

| 단계 | 점 추정 | 제안 단계 상한 | 추정 근거 |
|---|---:|---:|---|
| 재개·SSH 준비 | 90초 | 180초 | 관측80.447초, 별칭 갱신 포함 |
| graph 확인·CUDA build·co-deploy | 90초 | 260초 | 과거 배포37–39초와 graph9초, 추가 검산 여유 |
| 두 arm 스모크·status·quota | 210초 | 635초 | 과거 arm당50–54초; 상한은 기존 세부 timeout의 합 |
| 정리·증거 export | 45초 | 90초 | 새 실행은 미측정; 기존55+35초 timeout |
| 중지 요청·STOPPED 확인 | 420초 | 480초 | 관측383.676초를 포함한 예약; CLI 시간을 중복 가산하지 않음 |
| 합계 | **855초(14분15초)** | **1,645초(27분25초)** | 단일 관측에서 만든 계획이며 통계적 보장 아님 |

기존 900초에서 이미 소비한 보수적 관측 구간465.656초를 빼면 **434.344초**다.
중지 확인 예약480초만 반영해도 **45.656초 부족**하다. 새 900초를 배정해도
단계 상한 합보다745초 부족하며, 점 추정만으로는45초 여유여서 현재 미측정 범위를
수용하기 어렵다.

**제안은 새 30분(1,800초) 단위**다. 단계 상한 합 뒤155초(2분35초)의 여유를 둔다.
보존한 표시요금 $1.74/h 기준 시나리오 비용은 $0.87이다. 실제 청구액이나 승인된
추가 예산을 뜻하지 않는다. 480초 중지 예약도 단일 관측에 근거하며 provider SLA가 아니다.

## 다음 실제 실행 전 조건

이 디렉터리의 runner는 실제 현장 consumer 연결본의 snapshot이며, 새 30분 실행기가
완성됐다는 뜻은 아니다. 기존 900초/정리180초 예약은 이번 산정에 부족하다. 다음 배정에서
총예산과 단계별 deadline을 먼저 맞춰야 한다. 재개180초와 중지 확인480초를 누적 deadline으로
제한하고, 새 작업 시작 전에 정리90초+중지480초를 남겨야 한다. 기존의 최대40초 inventory
조회20회 반복을 중지 상한 대신 사용하면 안 된다. 별칭 갱신도 재개 준비 상한 안에 포함한다.

이번 로컬 단위는 현재 예산을 늘리거나 VM을 재개하지 않았다. 이전 실패 원문·측정 자료와
production CUDA 미실행 상태, 기존 검증 기준은 그대로 보존했다.

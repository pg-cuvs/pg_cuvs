## 변경 내용

- 데몬 `--rmm-pool-mb N` 옵션을 추가했습니다. 기본 0은 기존 allocator를 그대로 사용합니다. N은 allowed GPU마다 예약하는 초기 pool MiB이며 VRAM hard cap이 아닙니다.
- 하드웨어 probe, 인덱스 reload와 warmup, worker 시작보다 먼저 per-device RMM pool을 초기화합니다. 실패하면 daemon startup을 중단합니다. 실행 중 allocator 교체는 지원하지 않습니다.
- 재사용 가능한 RMM pool 공간을 VRAM admission 여유에 포함했습니다. 두 개의 atomic/alignment-aware counter로 pool 예약량과 실제 사용량을 추적합니다.
- 백엔드 `SHOW cuvs.rmm_pool_mb` / `current_setting()`으로 데몬의 실제 적용값을 읽습니다. PGC_INTERNAL 읽기 전용 상태이며, 데몬 응답을 받지 못하면 `unavailable`로 표시합니다. SQL 스키마/버전 변경은 없습니다.
- bootstrap의 `RMM_POOL_MB`, systemd EnvironmentFile, `make gpu-server-start RMM_POOL_MB=N` 전달 경로와 실행·메타데이터 수집 절차를 문서화했습니다.

## 검증

- `make test-unit`: util **253 passed**, build corpus **27 passed**. macOS에서는 Linux 전용 memfd/reaper 검사를 건너뜁니다.
- `make docs-contract-check`, `bash -n infra/brev/bootstrap.sh`, IPC/CPU shim C 문법 검사 통과.
- Linux/aarch64 PostgreSQL **16.15**, pgvector **0.8.0**의 CPU shim 확장·데몬 빌드 및 설치 통과.
- SQL 회귀 `rmm_pool`, `smoke` **2/2 통과**. 적용값 0, INTERNAL 상태, SQL SET 거부, daemon 미접속 상태를 0으로 오인하지 않는 동작을 확인했습니다.
- 동시성 격리 검사 **6/6 통과**.
- CLI 음수·잘못된 단위·overflow·누락 인수 거부 및 CPU shim에서 nonzero GPU pool을 적용했다고 거짓 보고하지 않는 startup 실패를 확인했습니다.
- `make -n gpu-server-start ... RMM_POOL_MB=1024`에서 옵션 전달 확인.

## 실제 GPU 검증

- 전용 Crusoe L40S 48GB(`sm_89`), driver 565.57.01, CUDA 12.4.131, cuVS/RMM 26.04.00에서 CUDA 빌드·동시 배포·preflight 통과.
- 첫 native server link는 `librapids_logger`의 DSO 누락으로 실패했습니다. `fded8ce`에서 명시적으로 링크하여 재빌드가 통과했습니다.
- baseline에서 한 번 만든 wiki 1M graph를 재사용했습니다. pool=0은 미리 정한 3회 측정 평균에서 candidates 64/128의 recall/serial p50 모두 고정 baseline 관측 범위 안에 들었습니다. 개별 지연 측정의 범위 이탈과 최초 gate failure도 내부 raw에 보존했습니다.
- 실제 daemon 적용값 1024를 SQL status로 확인한 뒤 같은 두 점의 GPU smoke를 완료했습니다. recall은 baseline 관측 범위 안이고 dispatch error는 없었습니다. 성능 수치는 별도 내부 기록이며 이 PR에서 일반적인 개선 배수를 주장하지 않습니다.
- pool1024 daemon의 quiescent shutdown은 `Result=success`, `ExecMainStatus=0`으로 종료됐습니다. 동시 요청 중 shutdown, multi-device 동작과 A100 그리드 전체 벤치마크는 검증하지 않았습니다.

RMM API는 저장소 bootstrap의 [RMM 26.04 source](https://github.com/rapidsai/rmm/tree/v26.04.00/cpp/include/rmm/mr)를 확인했습니다. GUC tuning PR과 별도이며 GUC merge 뒤 rebase가 필요합니다. `src/hnsw_export.c`는 변경하지 않았습니다.

## 기본값 검토(내부 제안)

실제 기본값은0이고 pool maximum은 미설정입니다. 별도 L40S 크기 비교는6크기를각3회 교차 실행했으며 initial512MiB부터 p50이 평탄했습니다. 제안은 initial512MiB + per-device total CUDA VRAM50%를 upstream quota로 제한하는 방식입니다. private native의검색/IVF-PQ/NN-descent에서 용량을 검산했고 현재 doubling 정책을 유지했습니다. 이 prototype은 production API에 반영되지 않았습니다. 최대값 전달·상태와 cap-aware admission,다중 index 동시성 검증이 필요합니다.

검색/빌드 원자료와 평균·표준편차,원실패는 로컬 bench/results/index-accel-20261021에 보존했습니다. 데이터 공개 단계까지 raw 커밋의 공개 push를 보류합니다.

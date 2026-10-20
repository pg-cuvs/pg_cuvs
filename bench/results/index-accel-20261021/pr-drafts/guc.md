## 변경 내용

- `cuvs.itopk_size`와 `cuvs.search_width`를 USERSET GUC로 추가했습니다. 기본값 0은 기존 자동 버퍼 계산과 SDK search width 기본값을 그대로 유지합니다.
- 명시한 intermediate buffer는 실제 dispatch의 k를 수용하도록 올립니다. shard/tombstone overfetch 때문에 잘못된 크기가 커널로 전달되는 것을 막습니다.
- 일반·필터·명시 배치·동적 배치·샤드 검색에 세션 설정을 전달하고, 서로 다른 CAGRA 설정의 요청을 같은 마이크로배치에 섞지 않습니다.
- IPC command frame을 v4로 갱신했습니다. 확장과 데몬을 함께 배포해야 합니다.

## 검증

- `make test-unit`: util **258 passed**, build corpus **27 passed**. macOS에서는 Linux 전용 memfd/reaper 검사를 건너뜁니다.
- `make docs-contract-check`: 통과.
- Linux/aarch64 PostgreSQL **16.15**, pgvector **0.8.0** 컨테이너에서 CPU shim 확장·데몬 빌드 및 설치 통과.
- SQL 회귀: `cagra_search_params`, `cagra_dynbatch`, `pg_cuvs_batch` **3/3 통과**. 기본값, 잘못된 값 거부, SET LOCAL rollback, 기본/설정 검색, microbatch와 explicit batch를 확인했습니다.
- 동시성 격리 검사 **6/6 통과**.

## 실제 GPU 검증

- 전용 Crusoe L40S 48GB(`sm_89`), driver 565.57.01, CUDA 12.4.131, cuVS/RMM 26.04.00에서 확장과 데몬의 CUDA 빌드·동시 배포·preflight 통과.
- baseline `8b73d65`에서 한 번 만든 wiki 1M graph를 재사용했습니다. GUC 기본값 0으로 candidates 64/128, 각 10,000 query의 recall과 serial p50이 미리 고정한 baseline restart 3회의 관측 범위 안에 들었습니다.
- graph/query/ground-truth SHA와 GPU dispatch/error 카운터를 검증했습니다. 동일 baseline에서도 결과 ID가 조금씩 달라져 byte equality는 기준으로 사용하지 않았습니다.
- A100(`sm_80`) 그리드 전체 벤치마크는 실행하지 않았습니다. #144 itopk sweep 결과는 별도 내부 기록에 둡니다.

`src/hnsw_export.c`는 변경하지 않았습니다. RMM pool은 별도 PR입니다. IPC v4 때문에 확장과 데몬을 함께 배포해야 합니다.

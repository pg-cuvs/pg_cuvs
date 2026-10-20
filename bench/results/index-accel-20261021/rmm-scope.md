# RMM 적용 범위와 성장·VRAM 회계

기준 source는 `fded8ce8fb289112349fbce0d36e2e85f47e9eab`, 설치 RMM은 26.04.00이다.

- `src/cuvs_wrapper.cu:179–200`: device마다 current resource를 usage counter → pool → upstream counter → 기존 MR 순서로 교체한다. 0이면 resource lookup/CUDA allocation 없이 반환한다.
- `src/pg_cuvs_server.c:8675–8689`: hardware probe, persisted-index reload, warmup, worker 시작 전에 초기화한다. 따라서 workspace만의 pool이 아니다.
- `src/cuvs_wrapper.cu:1074`의 retained dataset과 `:1113`의 CAGRA build, `:1293` 이후 batch query/output matrix 등 current MR을 쓰는 할당을 감싼다. 라이브러리가 MR을 우회해 직접 CUDA API로 할당한 메모리는 별도이며 계측에서 구분한다.
- `src/cuvs_wrapper.cu:193`: initial size만 constructor에 전달한다. SDK `pool_memory_resource.hpp:318–359`는 maximum 미설정일 때 새 chunk를 `max(request_bytes,current_pool_bytes)`로 요청한다. 보통 전체 pool이 두 배로 커지고, 실패하면 최소 요청까지 시도 크기를 절반씩 줄인다. 설정된 수치상 maximum은 없고 upstream 가용량/OOM이 한계다. 초기 MiB는 hard cap이 아니다.
- `pg_stat_gpu_search.vram_bytes`는 `src/pg_cuvs_server.c:648–658,6027–6031`의 dataset+graph 추정값이다. pool의 미사용 선점분과 검색/빌드 scratch는 index별 값에 포함되지 않는다.
- `src/cuvs_wrapper.cu:537–542`는 pool reserved−used를 CUDA free에 더해 admission이 재사용 가능 공간을 볼 수 있게 한다. `src/pg_cuvs_server.c:697–700,2095–2103`의 physical-headroom eviction과 별도 logical budget 추정을 함께 사용한다. 초기 선점량 전체를 index별 vram_bytes에 더하지 않는다.

행은 이 source snapshot 기준이다. 초기 pool의 할당 1회, 이후 pool 성장 호출, 직접 CUDA malloc 계열 호출 수는 피크 측정 표에서 따로 센다.

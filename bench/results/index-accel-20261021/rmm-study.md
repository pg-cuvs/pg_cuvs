# RMM 크기·피크·빌드 검증

이 L40S의 1M 검색에서 초기 pool은 512 MiB부터 지연이 거의 평탄했다. 제안은 initial512 MiB + per-GPU total CUDA VRAM의50%를 upstream quota로 제한하는 규칙이다. 현재 production source의 기본값0과 상한 미설정은 변경하지 않았다.

## 범위와 조건

- source `fded8ce`, L40S(sm89), CUDA12.4.131/cuVS·RMM26.04.00, PostgreSQL16.15. native CUDA 빌드와 preflight 통과. 첫 native server link의 rapids_logger DSO 누락은 fded8ce에서 고쳤고 원 실패 로그를 보존했다.
- 검색: 고정 wiki1M×768 graph(gd32/igd128/ivf_pq), 제공GT, 10,000 query. 크기0/256/512/1024/2048/4096 각각N=3, 총18trial을 교차 순서로 실행했다. 두k마다 PG/daemon 재시작, graph reload, heap/TOAST/index prewarm, 첫client100건 기록, warm100건, steady10000건을 측정했다.
- 첫100건은 daemon의 필수 hardware/index warmup 이후 client 요청이다. 순수 CUDA context 첫 할당과 다르며, 성장 호출은 별도 native probe에서 계측했다.
- 후보 기준은 두k 모두 positive-size 최저 p50 평균의5% 이내인 가장 작은 initial이다. 모든 trial을 포함했고 각 점10200 GPU query/error0, graph/query/GT SHA를 검증했다.
- 아래 지연과 CREATE INDEX는 계측 없는 PR 바이너리다. MR 통계/Nsight의 실행 시간은 이 표에 섞지 않았다. A100 그리드·native cuVS·PR #2872와 같은 경계의 성능 수치가 아니다.

## 1. 검색 크기 비교

평균±표본 SD, 각 크기/k의 N=3. 단위ms.

| initial MiB | p50 k64 | p50 k128 | 첫100 p99 k64 | 첫100 p99 k128 |
|---:|---:|---:|---:|---:|
| 0 | 0.971831 ± 0.001909 | 0.978364 ± 0.004202 | 1.146401 ± 0.011439 | 1.284737 ± 0.208283 |
| 256 | 0.905621 ± 0.030907 | 0.943347 ± 0.002334 | 0.995629 ± 0.043446 | 0.993482 ± 0.037809 |
| 512 | 0.802276 ± 0.001274 | 0.816421 ± 0.004614 | 0.982205 ± 0.016304 | 0.990497 ± 0.017059 |
| 1024 | 0.800390 ± 0.003709 | 0.812047 ± 0.001881 | 0.986101 ± 0.025743 | 1.023779 ± 0.066985 |
| 2048 | 0.800792 ± 0.002914 | 0.811902 ± 0.001982 | 0.970510 ± 0.015051 | 1.003289 ± 0.021264 |
| 4096 | 0.800876 ± 0.002233 | 0.822873 ± 0.008031 | 0.973222 ± 0.022445 | 1.010445 ± 0.022330 |

| initial MiB | recall@10 k64(N=3 평균±SD) | recall@10 k128(N=3 평균±SD) |
|---:|---:|---:|
| 0 | 0.94060333 ± 0.00001528 | 0.97844333 ± 0.00005132 |
| 256 | 0.94069333 ± 0.00004163 | 0.97843333 ± 0.00001528 |
| 512 | 0.94069667 ± 0.00004726 | 0.97851667 ± 0.00006429 |
| 1024 | 0.94065667 ± 0.00007024 | 0.97846000 ± 0.00001732 |
| 2048 | 0.94067000 ± 0.00003606 | 0.97843667 ± 0.00002517 |
| 4096 | 0.94073333 ± 0.00004163 | 0.97842667 ± 0.00004619 |

512 MiB p50 평균은 pool0보다 k64 17.45%, k128 16.55% 낮았다. 256 MiB는 두k에서 plateau 기준을 충족하지 못했다. 이는 이 하네스·GPU의 N=3 관측이다.

## 2. MR 피크와 pool 고수위

초기1024 MiB, maximum미설정의 참조 native 실행. 각 행N=1이라 SD는 산출하지 않았다(—). device MR의 위·아래에 statistics_resource_adaptor를 두었다. MR을 우회한 직접 CUDA 할당은 별도 열이다. GB는10^9bytes.

| 시나리오 | N/SD | live peak GB | 종료 pool GB | phase 성장 수 | 초기 포함 전체 upstream 수 | CUDA API 관측 |
|---|---|---:|---:|---:|---:|---|
| 검색 IT512 / Q10000 / K64 | 1 / — | 3.235880 | 4.145742 | 0 | 2 | cudaMallocAsync_ptsz_v11020=1 |
| build gd64/igd256 ivf_pq | 1 / — | 8.797118 | 16.582967 | 3 | 4 | cudaMalloc_v3020=3, cudaMallocManaged_v6000=3, cudaFree_v3020=3 |
| build gd64/igd256 nn_descent | 1 / — | 7.168000 | 8.291484 | 2 | 3 | cudaMalloc_v3020=2, cuMemFreeHost=9 |

검색 phase 전에 graph load가1회 성장했다. 검색 자체의 추가 live peak는35.88MB, 추가 pool 성장은0이다. SQL max_batch_queries4096 한계를 피해 peak의10000건 batch는 같은 wrapper를 직접 호출했다. private patch는 그 batch의 itopk512/search_width1만 고정했다.

pool은 current device resource라 graph 적재·retained dataset·build·검색의 RMM 할당을 감싼다. `src/cuvs_wrapper.cu:179–200`, `src/pg_cuvs_server.c:8675–8689`에서 worker/probe/reload보다 먼저 설치한다. initial은 hard cap이 아니다. 현재 SDK maximum미설정 시 chunk는 max(request,current_pool), 실패 시 요청량까지 절반씩 줄여 시도한다. 상세 행 근거는 [rmm-scope.md](rmm-scope.md)에 있다.

## 3. CREATE INDEX 벽시계와 VRAM 회계

별도 DB를 index_accel_smoke의 template으로 복제했다. 원 source graph는 보존했고 gd64/igd256/ivf_pq의 새 시험 index를 0/512 교차 순서로 각각3회 만들었다. 동일한 prewarm과 maintenance_work_mem8GB/workers7 조건이다. 표는 평균±표본 SD, N=3이다.

| initial MiB | CREATE INDEX 초 | NVML used GB | pg_stat_gpu_search 합계 GB | 물리−논리 GB |
|---:|---:|---:|---:|---:|
| 0 | 46.442748 ± 0.471304 | 3.962569 ± 0.000000 | 3.328000 ± 0.000000 | 0.634569 ± 0.000000 |
| 512 | 45.199552 ± 0.820798 | 15.069086 ± 0.000000 | 3.328000 ± 0.000000 | 11.741086 ± 0.000000 |

이번 평균 벽시계 감소는2.68%였다. 전체 그리드의 build-time headline으로 일반화하지 않는다.

index별 vram_bytes는 dataset+graph의 추정값이라 unused pool과 scratch를 포함하지 않는다. 물리 used에는 pool 선점분 외에 context/기타 cache도 포함된다. pool512의 pool0 대비 추가 물리 hold는 약11.107GB다. 내부 admission은 reserved−used를 physical free에 더해 재사용 공간을 인식하지만, index eviction이 그 pool의 물리 메모리를 다른 CUDA 사용자에게 반환하지는 않는다.

첫 cohort는 CREATE INDEX 뒤 float−Decimal collector 오류로 중단돼 walltime이 저장되지 않았다. 원 실패를 보존하고 숫자를 복원하지 않았다. 수정 뒤 새 build_r2_라벨의 성공6건이 위 N=3 자료다. timing.json을 CREATE 직후 저장하도록 고쳤으며, 실패1건을 유리한 outlier로 골라 버린 것이 아니다.

## 4. 반복 할당 호출

별도 native serial100(K64/IT64), 각N=1/SD—. profiler 시간은 지연 표에서 제외한다.

| initial MiB | upstream 할당 수 | cudaMalloc 수 | cudaMallocAsync 수 |
|---:|---:|---:|---:|
| 0 | 800 | 800 | 1 |
| 256 | 0 | 0 | 1 |
| 512 | 0 | 0 | 1 |
| 1024 | 0 | 0 | 1 |

반복 cudaMalloc 회피가 확인됐다. 256→512의 추가 지연 차이는 native에서 양쪽 모두 pool 성장/cudaMalloc이0이므로 이 자료로 원인을 확정하지 못했다. SQL 첫100건의 p99를 초기 pool 성장만의 시간으로 해석하지 않는다.

## 기본값 제안과 검산

제안: initial512 MiB + maximum=floor256(total CUDA VRAM×50%), per GPU. 상한은 upstream limiting_resource_adaptor의 quota로 강제하고 pool constructor의 maximum은 미설정으로 두어 현재 doubling 정책을 유지한다.

현재 PR의 실제 기본값은0이며 maximum 옵션은 아직 없다. 위 제안은 private prototype으로만 검산했다. CUDA total47.693955GB에서 cap23.846978GB였다. 각N=1/SD—인 다음 세 native 실행이 모두 성공했다.

| private quota50% 시나리오 | live peak GB | 종료 pool GB | phase 성장 수 |
|---|---:|---:|---:|
| search | 3.235880 | 3.608871 | 0 |
| ivf_pq | 9.365487 | 14.435484 | 3 |
| nn_descent | 7.168000 | 14.435484 | 3 |

후보의 최대 pool hold14.435484GB + 두 번째 gd64 index 추정3.328GB를 보수적으로 합해도17.763484GB로 cap 안에6.083494GB가 남는다. 더 큰 참조1024 고수위16.583GB를 쓴 합도 cap 안에3.936GB가 남는다.

이는 이 1M 셀의 용량 검산이다. 동시 multi-index workload와 다른 scale을 검증한 것은 아니다. production에 maximum을 넣을 때는 status/설정 전달과 함께 admission의 growable도 min(physical free+async reclaim, cap−reserved)로 제한해야 한다. SDK pool constructor에 finite maximum을 바로 전달하면 remaining/2 성장 정책으로 바뀌므로 별도 설계 선택이다.

## 원 자료

- pool-sweep-summary.csv / pool-sweep-raw.csv / raw/pool-sweep/: 36점의 metrics, ID, 첫100/steady10000의 query별 latency CSV.
- build-time-summary.csv / build-time-raw.csv / raw/build-times/: walltime, DDL, 설정, 물리/논리 VRAM raw.
- peak-profile/, allocation-profile/: Nsight 원 보고서·SQLite·CUDA memory events·실제 API count.
- bounded-results/와 bounded-quota.patch: cap prototype의 성공 여부와 source 변경.
- logs/와 diagnostics/: 첫 link·collector·byte gate 실패를 포함한 원 로그.
- remote-evidence-map.json의 원격574파일 약119.8MB를 로컬 SHA 검증했고, 원 graph의 최종SHA도 initial과 일치했다.

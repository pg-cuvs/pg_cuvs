# GUC 기본값과 #144: L40S 검증

이 L40S에서 같은 graph의 배치 recall 저하는 itopk128부터 3회 모두 관측됐다. 별도 Nsight trace의 실제 커널은 단일 검색 `multi_cta_search`, 배치1000 `single_cta_search`였다. 따라서 이 관측을 multi-CTA에 의한 recall 저하로 명명할 근거는 없다.

## 조건

- 전용 Crusoe L40S 48GB(sm89), 8vCPU, driver565.57.01, CUDA12.4.131, cuVS/RMM26.04.00, PostgreSQL16.15, pgvector0.8.7. 예정된 A100(sm80) 그리드와 GPU가 다르며 결론은 이 L40S에 한정한다.
- wiki_all_1M 1M×768, 제공 GT top10, 10000 query. baseline8b73d65에서 gd32/igd128/ivf_pq graph 한 번 생성 후 재사용. hash는 baseline-noise.json에 고정했다.
- 사설 pg_cuvs 하네스: 직렬 검색은 id+distance SQL, 배치는 pg_cuvs_batch_search와 heap TID join. native cuVS 또는 PR #2872의 QPS와 혼합하지 않는다.
- 회차마다 PG와 daemon을 재시작하고 heap/TOAST/index를 prewarm했다. 단일 검색은 점당 client100건 warmup 후10000건, 배치는1000×10 dispatch를 warmup2회/repeat3회 수행했다.

## GUC0 기본 동작

`0caa648`의 CUDA 빌드·확장/daemon 동시 배포·preflight가 통과했다. GUC 둘 다0에서 recall/p50이 feature 실행 전에 고정한 baseline3회 관측 span 안에 들었다. 이 span은 신뢰구간이 아니다.

| k | baseline recall 평균±SD(N=3) | GUC0 recall(N=1) | baseline p50 평균±SD ms(N=3) | GUC0 p50 ms(N=1) |
|---:|---:|---:|---:|---:|
| 64 | 0.94068000 ± 0.00006083 | 0.94065000 | 0.968332 ± 0.001968 | 0.967738 |
| 128 | 0.97844667 ± 0.00004726 | 0.97845000 | 0.999547 ± 0.002677 | 1.001589 |

GUC0는 N=1이라 SD를 산출하지 않았다. 각 점 GPU10000건/error0이며 planner가 t_cagra를 선택했다. 기본값0은 legacy itopk 계산과 SDK search width를 유지한다. IPC v4이므로 확장과 daemon을 함께 배포해야 한다. CPU util258+corpus27, SQL3, isolation6 및 docs check의 기록은 PR 초안에 있다.

## #144

아래 recall은 평균±표본 SD, N=3이다. returned k64, search_width1을 고정했다. 커널 열은 별도 단일1건/배치1000건 probe의 실제 symbol이며, 지연 측정 자체를 profile하지 않았다.

| itopk | single recall | batch recall | batch−single | single1 trace | batch1000 trace |
|---:|---:|---:|---:|---|---|
| 64 | 0.94063667 ± 0.00006429 | 0.94304000 ± 0.00000000 | +0.00240333 ± 0.00006429 | multi_cta | single_cta |
| 128 | 0.97838000 ± 0.00004583 | 0.97810000 ± 0.00000000 | -0.00028000 ± 0.00004583 | multi_cta | single_cta |
| 256 | 0.99336000 ± 0.00004359 | 0.99007000 ± 0.00000000 | -0.00329000 ± 0.00004359 | multi_cta | single_cta |
| 512 | 0.99783000 ± 0.00003606 | 0.99600000 ± 0.00000000 | -0.00183000 ± 0.00003606 | multi_cta | single_cta |

itopk 증가 자체에 따른 recall 하락은 없었다. 배치 결과 ID는 세 회차 모두 동일(Jaccard1.0)이었고 단일 검색의 회차 쌍 평균 Jaccard는 itopk64/128/256/512에서 0.99942491/0.99924444/0.99929217/0.99971111이었다. 각 점 single10000/error0, batch50dispatch/error0/fallback0이고 배치 실행 계획에 Seq Scan은 없었다.

기존 [issue144](https://github.com/pg-cuvs/pg_cuvs/issues/144)는 A100/K200 등의 다른 조건을 담고 있다. 동일 graph에서 batch가 낮아지는 현상은 이번 K64/itopk sweep에서도 관측됐으며, 기존 본문의 커널 명칭을 이번 GPU의 관측 모드로 대체해 추론하지 않는다.

## 보존한 실패와 제한

- 변경 없는 baseline 재시작에서도 ID 변경 query가 k64 30건, k128 50건이었다. k64는30건 모두 GT 밖 후보가 개입했고, k128은 순서만2건+GT 밖 후보48건이었다. 원 byte-equality gate exit1은 diagnostics/에 그대로 보존했다.
- 따라서 ID byte 일치는 성능/품질 실패 기준으로 사용하지 않았다. query·GT·graph SHA, recall, p50 및 Jaccard를 함께 기록했다.
- CUDA trace daemon은 exit0으로 정리됐으나 driver shutdown 중 CUDA event/async-free warning을 남겼다. 원 trace log를 보존했다.
- PR 생성은 .claude/pr-policy.json 미등록 훅으로 막혔으며 해당 정책의 직접 사용자 승인 답변은 아직 없다. 정책 파일을 생성하거나 차단을 우회하지 않았다.

## 원 자료

- baseline-noise.json, smoke-summary.csv, raw/baseline_control1..3, raw/guc_zero
- issue144-summary.csv(커널 포함), issue144-aggregate.json, raw/issue144_run1..3
- cta-profile/cta-modes.json: 전체 kernel 이름, nsys-rep, SQLite, NVTX 요청 조건
- harness/feature_gpu_check_smoke_frozen.py는 smoke8개 메타데이터 SHA와 정확히 일치한다. sweep 하네스에는 batch dispatch/fallback/plan 검증만 추가했다.

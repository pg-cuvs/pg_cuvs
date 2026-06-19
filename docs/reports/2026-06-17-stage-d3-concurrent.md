# 세션 보고서 — Stage D3 concurrent 검증 (CAGRA query-QPS-under-ingest)

| 항목 | 값 |
|------|-----|
| 날짜 | 2026-06-17 |
| 범위 | D3 incremental의 마지막 미측정 셀 — `forced-cuvs`(CAGRA) concurrent query-QPS-under-ingest |
| 런 | `gha-27665874191` (A100, real cuVS, Stage D / module=incremental) |
| 결과 | **CAGRA 757.8 → 9.8 qps, 98.7% degradation**. no-index 0% 저하, flat FAILED(사전 진단 finding 재현). status=OK, cells_done=3/3 |
| 산출물 | `bench-results/protocol:results/protocol/D.csv` (commit `4fa15d9`), HANDOFF.md 갱신 (commit `61464c8`) |

---

## 1. 요약 (TL;DR)

D3(incremental)에서 유일하게 "build=true VM 런 대기" 상태로 남아 있던 셀 —
**CAGRA가 백그라운드 스트리밍 ingest(`cuvsCagraExtend`) 중 쿼리 처리량을 얼마나
잃는가** — 를 A100 실측으로 채웠다.

결과는 명확하다. CAGRA 쿼리 처리량은 동시 ingest 하에서 **757.8 → 9.8 qps,
98.7% 붕괴**한다. 원인은 GPU 커널이 아니라 **데몬 내 `g_index_mutex` 단일 락이
extend↔search를 직렬화**하는 구조다. 이는 ADR-074의 **포지셔닝 가이드**(read-heavy→flat,
write-heavy→pgvector-무인덱스)를 정량적으로 확정한다 — 단 이는 *사용자가 인덱스 타입을
고르는 기준*이지 시스템이 쿼리마다 자동 전환하는 라우터가 아니다(§4.4 참조).

---

## 2. 측정 셀 / 구성

| 항목 | 값 |
|------|-----|
| 데이터셋 | cohere-1m, N=100k, dim=1024, k=10, recall_target=0.99 |
| 시나리오 | `PGCUVS_INC_SCENARIO=concurrent` — base 98k 로드 후, 쿼리 처리량을 (1) ingest 없이, (2) 백그라운드 INSERT 스레드 동시 실행 하에서 각각 측정 |
| 엔진 | `forced-cuvs`(CAGRA, 실 `cuvsCagraExtend`), `forced-flat`, `forced-noindex` |
| 하드웨어 | A100, real cuVS (build=true), `pg-cuvs-a100` self-hosted runner |
| 설계 | best-effort — 개별 셀 실패는 CSV `notes`로 surface하되 런은 실패시키지 않음 |

---

## 3. 결과

| config | index | baseline qps | under-ingest qps | degradation | 비고 |
|--------|-------|-------------:|-----------------:|------------:|------|
| `forced-cuvs` | **CAGRA** | 757.8 | 9.8 | **98.7%** | peak VRAM 384 MB, avg lat 253 µs, gpu 0.68 s |
| `forced-flat` | flat | — | — | **FAILED** | `delta sidecar unusable mid-scan; retry will replan to CPU` |
| `forced-noindex` | seqscan | 1.5 | 1.5 | 0% | CPU seqscan, GPU 경합 무관 |

`PGCUVS_RESULT: status=OK cells_done=3/3` — 3셀 모두 측정 완료(실패는 best-effort로 기록).

---

## 4. 해석

### 4.1 CAGRA — 동시 ingest 중 처리량 99% 붕괴 (핵심 finding)

CAGRA는 단독 쿼리 시 757.8 qps를 내지만, 백그라운드 INSERT 스레드(실 `cuvsCagraExtend`
스트리밍 빌드)가 동시에 돌면 **9.8 qps로 무너진다**. GPU 커널 자체의 문제가 아니다 —
peak VRAM 384 MB, GPU busy 0.68 s로 자원은 여유롭다. 병목은 **데몬의 단일
`g_index_mutex`가 extend와 search를 직렬화**해, 두 세션이 락을 두고 줄을 서는 데 있다.
즉 동시성 한계는 메모리/연산이 아니라 **락 구조**다.

### 4.2 no-index — 0% 저하, 단 절대값 1.5 qps

seqscan 경로라 GPU 데몬을 거치지 않아 ingest와 경합하지 않는다(0% 저하). 그러나 절대
처리량이 1.5 qps로, "동시성에 강하다"가 아니라 "애초에 인덱스 가속이 없다"는 뜻이다.

### 4.3 flat — 사전 진단된 FAILED finding 재현 (회귀 아님)

flat은 `cuvsCagraExtend`가 없어(ADR-073 BF-only, `handle==NULL`) INSERT가 매번 `.delta`
사이드카로 append된다(cagra는 GPU extend라 `.delta`가 churn하지 않아 미발현). 정밀
근본원인(**ADR-077**): writer `cuvs_delta_append`는 `flock LOCK_EX`인데 scan-time delta
reader는 **무락**이라, append 중간 상태(파일 크기↔헤더 `n_rows` 불일치)를 torn-read →
`delta sidecar unusable mid-scan → replan-to-CPU`. **정확성 버그가 아니라 가용성/race**다
(순차 정합성은 ADR-047 isolation 2/2 + D3 recall-drift 1.0으로 입증). 이번 변경으로 생긴
회귀가 아니며, 수정은 reader에 `LOCK_SH` 추가(ADR-077, ROADMAP 백로그).

### 4.4 "라우팅"의 정확한 의미 — 자동 전환은 (아직) 없다

§1·§5의 "read-heavy→flat, write-heavy→no-index"는 **세 층을 구분**해야 한다:

| 층 | 주체 | 현 상태 |
|----|------|---------|
| 인덱스 타입 선택 (`CREATE INDEX USING flat`/`cagra`/없음) | **사람(DDL)** | 의도된 운용 모델 (ADR-074 포지셔닝) |
| 플랜타임 cost 선택 | 플래너 | **존재하는** 인덱스 중에서만 고름 |
| 쿼리-시 cross-family 자동 전환 (flat↔cagra↔무인덱스) | 시스템 | **live 아님** — transient B `auto` 승격을 측정 후 의도적으로 안 함(교차점 없음, regret>0; ADR-073/069) |

즉 flat과 cagra는 **exact vs approximate**라는 직교 축이고(대체재 아님), 한 컬럼에 둘을
동시에 얹는 것도 비의도다. "write-heavy→no-index"는 *시스템이 자동으로 그렇게 한다*가
아니라 *그 워크로드엔 GPU 인덱스를 만들지 말라는 가이드*다.

---

## 5. 결론 / 후속

- **D3(incremental) GPU 런 대기 항목 종결.** CAGRA concurrent 셀이 측정되어 Stage D에는
  더 이상 build=true VM 런을 기다리는 incremental 셀이 없다.
- **ADR-074 포지셔닝 정량 근거 확보** — *인덱스 선택 가이드*로서 read-heavy는 flat,
  write-heavy는 pgvector-무인덱스. CAGRA는 동시 ingest+query 워크로드에 부적합(98.7%
  저하)함이 실측으로 확정됐다. (자동 cross-family 라우팅은 미구현 — §4.4.)
- **엔지니어링 후속(벤치 범위 밖)** — `g_index_mutex`의 extend↔search 직렬화를 reader/writer
  또는 더블버퍼링으로 완화하면 CAGRA의 동시 처리량을 회복할 여지가 있다. 별도 설계(ADR)로
  escalate.

---

## 부록 — 재현

```
PGCUVS_STAGE=D PGCUVS_MODULE=incremental \
PGCUVS_CELLS='N=100k;dim=1024;k=10;recall=0.99' \
PGCUVS_CONFIGS=forced-cuvs-concurrent,forced-flat-concurrent,forced-noindex-concurrent \
PGCUVS_DATASET=cohere-1m PGCUVS_INC_SCENARIO=concurrent \
bash bench/protocol/run.sh
```

원시 결과: `bench-results/protocol:results/protocol/D.csv` (run_id `gha-27665874191-1`),
런 로그 아티팩트 `bench-log-27665874191`.

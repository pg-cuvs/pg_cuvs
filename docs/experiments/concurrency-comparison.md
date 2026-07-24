# 동시성 대조 — GPU 벡터 인덱스에 동시 insert+query를 업계는 어떻게 푸나

> 외부 시스템 조사: FAISS · Milvus · OpenSearch · Kinetica의 cuVS/RAFT 통합과
> "동시 insert+query 시 GPU flat/인덱스 정합성" 처리. 2026-06 웹 조사(병렬 서브에이전트).
> 목적: pg_cuvs flat의 동시-쓰기 race(ADR-077)를 업계 패턴에 비춰 **우리 설계 정당성과
> fix 방향**을 확정한다. **결론: 우리 고수준 아키텍처는 업계 표준 그대로이고, 버그는 tail을
> 파일로 구현하며 reader 락을 빠뜨린 국소 결함이다.**
>
> 신뢰도: GitHub 이슈/wiki 원문은 직접 확인(확정). 다수 공식 docs(milvus.io, zilliz.com,
> docs.rapids.ai, kinetica.com, opensearch.org, AWS)는 사이트 WAF로 직접 fetch가 403 →
> WebSearch가 추출한 **1차 출처 본문 발췌** 기반(높은 신뢰, 단 코드 레벨 미확인).

## TL;DR — 아무도 GPU 인덱스를 동시 읽기 중 in-place로 변형하지 않는다

| 시스템 | 동시 insert+query 전략 | GPU/cuVS 위치 | tail(신선분) 처리 |
|--------|----------------------|--------------|------------------|
| **FAISS** | **안 풂 — 호출자 직렬화.** GPU 인덱스 읽기조차 non-thread-safe, `add`는 재할당(in-place 아님) | build+serving (cuVS v1.10+: Flat/IVF/CAGRA) | 없음(라이브러리) |
| **Milvus** | **growing(가변)+sealed(불변) segment** fan-out 후 머지 | sealed 단위 **불변 빌드**, build+serving (Knowhere+cuVS: CAGRA/IVF/BRUTE_FORCE) | growing=CPU brute-force, 삭제=tombstone bitmap, MVCC=GuaranteeTs |
| **OpenSearch** | **Lucene 불변 segment.** GPU는 serving 경로에 **없음** | **BUILD 전용** — CAGRA GPU 빌드→HNSW 변환→**CPU serving** | 신규=새 불변 segment, merge 리빌드 |
| **Kinetica** | **불변 CAGRA(수동 REFRESH)+미인덱스 brute-force 병행** | build+serving (RAFT→cuVS 이행 중) | 미인덱스 tail=brute-force, 동시성=컬럼나 MVCC |

## 시스템별 (원문 근거)

### FAISS — 라이브러리, 호출자가 직렬화
- GPU 인덱스는 **읽기조차 thread-safe 아님**: *"Faiss GPU indices are not thread-safe, even for read only functions"* (`StandardGpuResources` temp memory가 한 번에 한 사용자).
- 정합성은 **전적으로 호출자**: *"A multithreaded use of functions that change the index needs to implement mutual exclusion."* MVCC·내부 락·자동 리빌드 없음.
- flat `add`는 **in-place 아님** — `FlatIndex.cu`가 용량 초과 시 device 버퍼 **재할당**(소스 주석: *"The above may have caused a reallocation"*) → 동시 검색 중 포인터/텐서 뷰 무효화 위험.
- cuVS(v1.10+, Flat/IVF/CAGRA build+search)는 **동시성 모델 불변**(여전히 비보장).

### Milvus — growing/sealed segment + fan-out 머지
- 신규 insert는 가변 **growing segment**(in-memory)로 → flush 시 **sealed segment**(불변, object storage)로 굳고 **비동기 인덱스 빌드**. 쿼리는 growing(CPU brute-force/interim index)+sealed(ANN)를 검색해 topk reduce. **sealed 불변이라 "쓰기 중 인덱스 변형" 문제 자체가 없음.**
- 삭제=**delta binlog tombstone**→검색 시 bitset로 제외(인덱스 불변). 정합성=**GuaranteeTs/consistency level(MVCC)**.
- **Knowhere**가 FAISS/hnswlib+**cuVS/RAFT**를 `Serialize()/Load()`로 래핑. GPU 인덱스(GPU_CAGRA/IVF_FLAT/IVF_PQ/**BRUTE_FORCE**)는 **sealed 단위 불변 빌드**. v2.6 하이브리드(`adapt_for_cpu`: GPU 빌드→CPU HNSW serving)도 추가.

### OpenSearch — GPU는 빌드만, CPU가 serving
- **Lucene 불변 segment** 위 동작 — 신규 문서=새 불변 segment append, merge 리빌드.
- **cuVS=인덱스 BUILD 가속 전용** (RFC #2293: *"The GPU nodes will only be building the vector index"*). CAGRA 그래프는 **직렬화 전 HNSW(IndexHNSWCagra)로 변환→CPU 검색** (*"...converted to an HNSW-based graph, which can be searched on a CPU"*). **GPU online serving 없음.**
- Remote build: flush/merge 시 segment 벡터를 object store 경유 **전용 GPU fleet**에 보내 segment 단위 빌드→다운로드해 Lucene Directory에 배치(RFC #2294).

### Kinetica — 불변 CAGRA(수동 refresh) + 미인덱스 brute-force
- CAGRA 인덱스는 **자동 유지 안 됨, 수동 REFRESH 필요** (*"A CAGRA index is not automatically maintained and must be refreshed manually"*) → 배치 리빌드, in-place 아님.
- **인덱싱된 데이터=인덱스 검색 + 미인덱스 신규 벡터=brute-force(exact) 병행** (*"combining brute-force search for new, unindexed vectors with index-based search for stored data"*). 동시성은 컬럼나 DB 스냅샷/MVCC에 위임(벡터 한정 모델은 자료 부족).
- cuVS는 RAFT에서 이행 중(build+serving 둘 다 GPU).

## 공통 패턴 — 불변 인덱스 + brute-force tail + fan-out 머지

DB급 셋(Milvus·OpenSearch·Kinetica)이 수렴:
1. GPU/ANN 인덱스를 **불변 단위**(segment/스냅샷)로 빌드 — **제자리 변형 안 함**.
2. 신선 데이터는 **분리된 가변 tail** — CPU brute-force 또는 새 segment.
3. 쿼리는 (불변 인덱스 + tail) **fan-out 후 머지**.
4. 삭제는 **tombstone/bitmap** 오버레이.
5. tail 정합은 엔진 **MVCC/스냅샷**.

## pg_cuvs 대조 — 이미 같은 패턴, tail의 동시성 원시 자료구조만 약했다

| 패턴 요소 | 업계 | pg_cuvs |
|-----------|------|---------|
| 불변 인덱스 단위 | sealed segment | 빌드된 `.vectors`/CAGRA (base 세대 `base_tids_crc`) |
| 가변 tail | growing/미인덱스 | **`.delta` 사이드카** |
| tail 검색 | CPU brute-force | delta CPU-exact 머지(= tail brute-force) |
| fan-out 머지 | segment별 topk reduce | base+delta 머지 |
| 삭제 | tombstone bitmap | `.tombstone` + heap recheck |
| 정합성 | 엔진 MVCC | heap MVCC recheck |

**pg_cuvs의 고수준 아키텍처는 업계 표준 그대로다 — 틀린 길이 아니었다.** 유일한 차이는
**tail을 무엇으로 구현했나**:
- 업계: tail = **proper하게 동기화된 in-memory 구조**(Milvus streaming node + MVCC).
- pg_cuvs: tail = **파일(`.delta`)**, writer는 `flock LOCK_EX`인데 reader 무락 → **torn read**(ADR-077).

→ 우리 버그는 "잘못된 아키텍처"가 아니라 **표준 패턴의 tail을 파일로 구현하며 reader 락을
빠뜨린 국소 결함**. ADR-077의 reader `LOCK_SH`가 그 격차를 메워 tail을 업계의 in-memory tail과
같은 동시-안전 속성으로 끌어올린다.

## 두 가지 재해석

1. **CAGRA 98.7% 저하(`cuvsCagraExtend` under query)는 업계가 의도적으로 안 하는 경로다.**
   누구도 live-serving CAGRA 그래프를 동시 쿼리 중 extend하지 않는다 — **새 불변 segment를 빌드**
   한다. "concurrent write + GPU 인덱스"의 업계 정답은 *"extend를 빠르게"가 아니라 "serving
   인덱스를 건드리지 말고 불변 단위 + tail brute-force"*. 우리 `forced-cuvs-concurrent`는
   업계가 회피하는 안티패턴을 측정한 셈(D3 보고서 §4.3 보강).
2. **delta tail의 "sealing"이 약하다.** 업계는 tail이 차면 **백그라운드 비동기로 새 불변 segment
   빌드**(Milvus flush, OpenSearch merge) 또는 수동 REFRESH(Kinetica). pg_cuvs는 delta가
   cap(`cuvs_max_delta_rows`)에 닿으면 stale→REINDEX(수동) — 백그라운드 자동 compaction 부재가
   향후 개선 여지(**delta 자동 sealing**, 트리거 백로그 후보).

## 출처 (1차 우선)

**FAISS**
- wiki Threads/async: https://github.com/facebookresearch/faiss/wiki/Threads-and-asynchronous-calls
- wiki Running on GPUs: https://github.com/facebookresearch/faiss/wiki/Running-on-GPUs
- wiki GPU Faiss with cuVS: https://github.com/facebookresearch/faiss/wiki/GPU-Faiss-with-cuVS
- 소스 `FlatIndex.cu`: https://github.com/facebookresearch/faiss/blob/main/faiss/gpu/impl/FlatIndex.cu
- Meta Eng (2025-05-08): https://engineering.fb.com/2025/05/08/data-infrastructure/accelerating-gpu-indexes-in-faiss-with-nvidia-cuvs/

**Milvus**
- Data processing/segment: https://milvus.io/docs/data_processing.md
- Performance FAQ(growing brute-force): https://milvus.io/docs/performance_faq.md
- 삭제 메커니즘(delta/bitmap): https://milvus.io/blog/2022-02-07-how-milvus-deletes-streaming-data-in-distributed-cluster.md
- Consistency(GuaranteeTs/MVCC): https://milvus.io/docs/consistency.md
- GPU index overview / CAGRA: https://milvus.io/docs/gpu-index-overview.md · https://milvus.io/docs/gpu-cagra.md
- Knowhere: https://github.com/milvus-io/knowhere
- cuVS Milvus 통합: https://docs.rapids.ai/api/cuvs/stable/integrations/milvus/

**OpenSearch**
- RFC #2293 (build-only, CAGRA→HNSW): https://github.com/opensearch-project/k-NN/issues/2293
- RFC #2294 (remote build): https://github.com/opensearch-project/k-NN/issues/2294
- Meta #2391 (3-컴포넌트): https://github.com/opensearch-project/k-NN/issues/2391
- AWS GPU vector indexing: https://docs.aws.amazon.com/opensearch-service/latest/developerguide/gpu-acceleration-vector-index.html
- 블로그: https://opensearch.org/blog/gpu-accelerated-vector-search-opensearch-new-frontier/

**Kinetica**
- cuVS Kinetica 통합: https://docs.rapids.ai/api/cuvs/nightly/integrations/kinetica/
- Vector search docs: https://docs.kinetica.com/7.2/vector_search/
- Indexes(CAGRA 수동 refresh): https://docs.kinetica.com/7.2/concepts/indexes/
- 기능 페이지(brute-force+indexed): https://www.kinetica.com/features/vector-search/

---

**관련**: ADR-077(flat reader 락 — 본 대조가 fix를 정당화), ADR-047(delta/tombstone = 우리의
tail), ADR-073(flat AM), ADR-074(detoast 병목·포지셔닝). 자매 문서 `docs/experiments/pgstrom-comparison.md`
(PG-Strom OLAP 대조 — transient vs warm 상주). 측정: `docs/reports/2026-06-17-stage-d3-concurrent.md`.

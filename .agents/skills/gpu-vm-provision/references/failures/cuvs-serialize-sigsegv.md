# cuvs::neighbors::cagra::serialize → SIGSEGV (RESOLVED)

> **상태**: 해결됨 (cuVS 버그 아니라 우리 wrapper의 메모리 라이프타임 버그 + search_params 제약 위반 두 개가 함께 보였음).
> 처음 추적 당시 cuVS 버그로 오해해서 25.10 다운그레이드를 검토했으나 불필요. cuVS 26.04 그대로 사용 가능.

## 원래 증상

데몬이 CAGRA build 직후 SIGSEGV로 죽음.

저널:
```
[handle_build] cuvs_cagra_build OK
[handle_build] cuvs_cagra_serialize(/tmp/cuvs_indexes/<db>_<idx>.cagra)...
pg-cuvs-server.service: Main process exited, code=dumped, status=11/SEGV
```

파일 시스템:
```
/tmp/cuvs_indexes/<db>_<idx>.cagra    # 0 byte 파일 남음
```

`try/catch`도 안 잡힘 → 시그널 핸들러 레벨에서 죽음. 스택 손상 또는 nullptr deref.

## 진짜 원인 (Bug #1 — Dataset 라이프타임)

`cuvs_cagra_build`에서 `d_corpus`(device matrix)를 함수 로컬 변수로 두고 view만 cagra::build에 넘김:

```cpp
extern "C" CuvsCagraIndex cuvs_cagra_build(...) {
    raft::device_resources res;
    auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_vecs, dim);
    raft::copy(d_corpus.data_handle(), vecs, n_vecs * dim, res.get_stream());

    auto idx = cuvs::neighbors::cagra::build(
        res, params,
        raft::make_const_mdspan(d_corpus.view()));   // ← view만 전달

    return new CuvsCagraIndexImpl(std::move(idx));   // d_corpus는 여기서 소멸
}
```

함수 종료 → `d_corpus` 소멸 → device 메모리 해제 → `idx`는 **dangling pointer 상태**.

`serialize(include_dataset=true)`가 dangling 메모리를 직렬화하려다 SIGSEGV.

CAGRA index는 build 결과 **그래프**는 own하지만 **dataset은 입력의 view만 보유**한다. 입력 데이터셋은 index보다 오래 살아야 한다.

### 수정

`d_corpus`를 `CuvsCagraIndexImpl`의 owned member로 옮김:

```cpp
struct CuvsCagraIndexImpl {
    /* dataset은 idx보다 먼저 선언 → 소멸 시 idx 먼저 destroy, dataset 나중에 */
    raft::device_matrix<float, int64_t> dataset;
    cuvs::neighbors::cagra::index<float, uint32_t> idx;

    CuvsCagraIndexImpl(raft::device_matrix<float, int64_t> &&d,
                       cuvs::neighbors::cagra::index<float, uint32_t> &&i)
        : dataset(std::move(d)), idx(std::move(i)) {}
};

extern "C" CuvsCagraIndex cuvs_cagra_build(...) {
    raft::device_resources res;
    auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_vecs, dim);
    raft::copy(d_corpus.data_handle(), vecs, n_vecs * dim, res.get_stream());

    auto idx = cuvs::neighbors::cagra::build(res, params,
        raft::make_const_mdspan(d_corpus.view()));

    /* 명시적 재바인딩 — view가 우리가 보존하는 d_corpus를 가리키도록 */
    idx.update_dataset(res, raft::make_const_mdspan(d_corpus.view()));
    res.sync_stream();

    return new CuvsCagraIndexImpl(std::move(d_corpus), std::move(idx));
}
```

핵심 포인트:
- `dataset` 멤버를 `idx` 멤버보다 **위에** 선언 → C++ destruction 순서가 역순이라 `idx`가 먼저 소멸, `dataset`이 그 후 소멸 → idx 소멸 시점에 dataset이 아직 살아있음
- `update_dataset()`로 view를 명시적으로 재바인딩

## 추가 발견 (Bug #2 — search_params 제약 위반)

SIGSEGV를 고친 후에도 search가 0 rows 반환하며 status 1 (OOM_FALLBACK). 디버그 로그 추가하니:

```
RAFT failure at search_multi_cta.cuh line=195:
  `num_cta_per_query` (2) * 32 must be equal to or greater than `topk` (100)
  when 'search_mode' is "multi-cta".
  (`num_cta_per_query` = max(`search_width`, ceildiv(`itopk_size`, 32)))
```

CAGRA의 multi-cta 검색 모드 제약:
- `num_cta_per_query × 32 ≥ top_k`
- 기본 `itopk_size=64`, `search_width=1` → `num_cta_per_query = max(1, 2) = 2`
- 최대 허용 top_k = 64

우리 `pg_cuvs.c`는 `top_k=100` 하드코딩. 64 < 100 → 제약 위반.

### 수정

`cuvs_cagra_search`에서 `top_k`에 따라 `itopk_size`를 32 배수로 올림:

```cpp
cuvs::neighbors::cagra::search_params sparams;
int itopk = ((top_k + 31) / 32) * 32;
if (itopk < 64) itopk = 64;
sparams.itopk_size = itopk;
```

## 검증

수정 후 동작 확인:
- BUILD: `include_dataset=true` 직렬화 성공 (파일 크기 2068 bytes, dataset 포함)
- SIGTERM: 모든 인덱스 직렬화 후 종료
- 재시작: `.cagra` 파일 deserialize → 인덱스 복원
- SEARCH on deserialized index: in-memory와 **동일한 결과**

```sql
-- in-memory CAGRA
SELECT id, embedding <-> '[1,0,0,0]'::vector AS dist FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;
 id | dist
----+------
  1 |    0
  8 |    1
  5 |    1

-- daemon restart 후 deserialized CAGRA
[같은 결과]
```

## 교훈

1. **cuVS API의 dataset 소유권 모델**: CAGRA index는 그래프만 own하고 dataset은 view를 보유. 입력 데이터셋은 index 라이프타임 동안 alive해야 함. `update_dataset()` API로 명시적 바인딩 가능.

2. **C++ destruction 순서**: 구조체 멤버는 선언의 **역순**으로 소멸. dataset이 idx보다 오래 살게 하려면 dataset을 **위에** 선언.

3. **SIGSEGV vs 예외**: SIGSEGV는 시그널이라 `try/catch`로 못 잡음. 프로세스 종료 전 stack frame을 보기 위해 cuvs/raft는 자체 backtrace를 stderr로 찍어줌. journalctl로 확인 가능.

4. **CAGRA search_params 제약**: multi-cta 모드는 `num_cta_per_query × 32 ≥ top_k`. `itopk_size`를 top_k에 맞춰 동적 조정 필요.

5. **디버깅 전략**: 처음에 cuVS 버그로 오해. 격리된 가설 검증(`include_dataset=false` 한 줄 변경)으로 진짜 원인을 빠르게 좁혔음. 검증 가능한 최소 변경 → 결과 비교 → 다음 가설.

## 관련 파일

- `src/cuvs_wrapper.cu` — 수정된 wrapper
- `src/pg_cuvs.c` — top_k 사용처 (Phase 2에서 planner LIMIT 통합 예정)

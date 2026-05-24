# cuVS 25.x/26.x API Quirks

24.x 기반 코드/문서를 26.x로 옮길 때 마주치는 변경 사항.

## 패키지 통합

| 24.x | 26.x |
|------|------|
| `libcuvs` + `libcuvs-dev` | `libcuvs` (헤더 포함, dev 패키지 없음) |
| `cuvs/neighbors/cagra_serialize.hpp` | `cuvs/neighbors/cagra.hpp`에 통합 |

```cpp
// 24.x
#include <cuvs/neighbors/cagra.hpp>
#include <cuvs/neighbors/cagra_serialize.hpp>

// 26.x
#include <cuvs/neighbors/cagra.hpp>  /* serialize/deserialize merged here */
```

## include path 변경

cuVS 25.x+는 libcudacxx를 conda env 내 `include/rapids/` 하위에 둠.

```makefile
CUVS_RAPIDS_INCLUDE ?= $(CUVS_PREFIX)/include/rapids
PG_CPPFLAGS = -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src
```

## raft::device_resources — non-movable

24.x는 move/copy가 가능했지만 25.x+는 둘 다 deleted.

```cpp
// 24.x (가능했음)
struct MyIndex {
    cagra::index<float, uint32_t> idx;
    raft::device_resources res;  // 값으로 보관 OK
    MyIndex(cagra::index<...> &&i, raft::device_resources &&r)
        : idx(std::move(i)), res(std::move(r)) {}
};

// 26.x 패턴 — res는 멤버로 보관 X, 함수마다 새로 만듦
struct MyIndex {
    cagra::index<float, uint32_t> idx;
    explicit MyIndex(cagra::index<float, uint32_t> &&i) : idx(std::move(i)) {}
};

extern "C" int my_search(MyIndex *idx, ...) {
    raft::device_resources res;  // 매 호출 시 생성
    // ...
    res.sync_stream();
}
```

**비용**: 매 호출마다 `res` 생성/소멸 → CUDA stream 새로 만듦. 작은 오버헤드지만 hot loop에서 신경 써야 함. 필요하면 thread-local 풀로 우회 가능.

## RAFT 빌드 매크로

raft 26.x는 endian 매크로를 외부 정의 요구. 정의 안 하면 mdspan numpy 직렬화 코드에서 컴파일 에러.

```makefile
NVCC_FLAGS += -DRAFT_SYSTEM_LITTLE_ENDIAN=1
```

(x86_64는 항상 little endian이므로 1로 하드코딩 OK)

## brute_force API deprecated

`cuvs::neighbors::brute_force::build/search`의 일부 시그니처가 26.x에서 `[[deprecated]]`. 컴파일 경고만 나오고 동작은 함. 새 시그니처는:
```cpp
// 새로운 권장 형태 (cuvs/neighbors/brute_force.hpp 참조)
// build/search가 metric을 별도 인자로 받는 새 오버로드 사용 권장
```

당장 마이그레이션 필요 없음. 경고 무시 가능.

## CAGRA serialize SIGSEGV (미해결)

26.04의 `cuvs::neighbors::cagra::serialize`는 호출 시 SIGSEGV.

**관찰된 동작**:
- 파일은 생성됨 (0바이트)
- 함수에서 반환 안 됨 (try/catch도 안 잡힘 → 시그널 핸들러로 죽음)
- 데몬 프로세스 전체 종료

**대응**:
1. Phase 1 데모에서는 호출 비활성화 (메모리 내 인덱스만 사용)
2. 추후 25.10으로 다운그레이드 시도 (pgstrom-results에서 작동 확인된 버전)
3. cuVS C API (`cuvsCagraSerialize`) 시도 — C++ 템플릿보다 안정적일 수 있음
4. cuVS GitHub issue 검색/신고

## CAGRA build 작은 데이터 경고

데이터가 너무 작으면 (n_vecs < 64 등) 다음 경고가 정상 출력. 무시 OK:
```
[warning] Intermediate graph degree cannot be larger than dataset size, reducing it to N
[warning] Graph degree (64) cannot be larger than intermediate graph degree (N-1)
```

기본 `graph_degree=64, intermediate_graph_degree=128`이 작은 데이터엔 과함. 작은 테스트엔 인자로 줄여줘도 됨:
```cpp
cuvs::neighbors::cagra::index_params params;
params.graph_degree = std::min(64ul, n_vecs);
params.intermediate_graph_degree = std::min(128ul, n_vecs);
```

## CAGRA index ID 타입

`cuvs::neighbors::cagra::index<float, uint32_t>` — 인덱스 ID가 `uint32_t`. 64억 개 이상의 벡터를 한 인덱스에 넣으면 오버플로우. Phase 1/2 규모(10M~수억)에선 충분.

`int64_t`로 바꾸려면 별도 빌드 변형 필요 (지원 여부 확인).

## CAGRA dataset 라이프타임 (중요)

CAGRA `index<T, IdxT>`는 **그래프만 own**하고 **dataset은 view를 보유**한다. build에 넘긴 dataset 메모리가 index보다 먼저 해제되면 search/serialize 시 SIGSEGV.

[BAD] 잘못된 패턴 (Phase 1 초기 버그):
```cpp
extern "C" CuvsCagraIndex cuvs_cagra_build(...) {
    raft::device_resources res;
    auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_vecs, dim);
    raft::copy(d_corpus.data_handle(), vecs, ...);
    auto idx = cuvs::neighbors::cagra::build(res, params,
        raft::make_const_mdspan(d_corpus.view()));
    return new CuvsCagraIndexImpl(std::move(idx));  // d_corpus 소멸 -> idx dangling
}
```

[GOOD] 올바른 패턴:
```cpp
struct CuvsCagraIndexImpl {
    /* dataset을 idx보다 위에 선언 — destruction 순서가 역순이라
     * idx 소멸 시점에 dataset이 아직 살아있어야 함 */
    raft::device_matrix<float, int64_t> dataset;
    cuvs::neighbors::cagra::index<float, uint32_t> idx;

    CuvsCagraIndexImpl(raft::device_matrix<float, int64_t> &&d,
                       cuvs::neighbors::cagra::index<float, uint32_t> &&i)
        : dataset(std::move(d)), idx(std::move(i)) {}
};

extern "C" CuvsCagraIndex cuvs_cagra_build(...) {
    raft::device_resources res;
    auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_vecs, dim);
    raft::copy(d_corpus.data_handle(), vecs, ...);
    auto idx = cuvs::neighbors::cagra::build(res, params,
        raft::make_const_mdspan(d_corpus.view()));

    /* 명시적 재바인딩 — 우리가 보존하는 d_corpus를 view 대상으로 못박음 */
    idx.update_dataset(res, raft::make_const_mdspan(d_corpus.view()));
    res.sync_stream();

    return new CuvsCagraIndexImpl(std::move(d_corpus), std::move(idx));
}
```

자세한 증상/디버깅 과정: `failures/cuvs-serialize-sigsegv.md`.

## CAGRA search_params 제약 (multi-cta 모드)

`cuvs::neighbors::cagra::search`의 기본 `algo=AUTO`는 작은/중간 데이터에서 multi-cta 모드를 선택. multi-cta는 다음 제약:

```
num_cta_per_query x 32 >= top_k
num_cta_per_query = max(search_width, ceildiv(itopk_size, 32))
```

기본 `itopk_size=64, search_width=1` -> `num_cta=2` -> `top_k <= 64`.

`top_k > 64` 요청 시:
```
RAFT failure at search_multi_cta.cuh line=195:
  `num_cta_per_query` (2) * 32 must be equal to or greater than `topk` (100)
  when 'search_mode' is "multi-cta".
```

해결: `itopk_size`를 32 배수로 올림.
```cpp
cuvs::neighbors::cagra::search_params sparams;
int itopk = ((top_k + 31) / 32) * 32;
if (itopk < 64) itopk = 64;
sparams.itopk_size = itopk;
```

또는 `algo`를 명시적으로 single-cta로 (제약 없음, 작은 batch에 적합).

## device_resources 직접 생성의 함정

`raft::device_resources res;` 호출 시 내부적으로:
- 새 CUDA stream 생성
- 메모리 풀 초기화
- workspace 메모리 할당

여러 스레드/요청이 동시에 생성하면 GPU 측면에서 오버헤드. Phase 1 데몬은 단일 mutex로 직렬화하므로 문제 없음. Phase 2 멀티 스레드 GPU 워크에서는 풀링 필요.

## cuVS 버전 다운그레이드 절차 (필요할 경우)

26.04로 Phase 1 완전 검증됨 — 일반적으론 다운그레이드 불필요. 새 버전에서 회귀 발견 시 fallback용:

```bash
ssh ubuntu@<IP> "
source ~/miniforge3/bin/activate
conda create -n cuvs_dev_25 -y \
    -c rapidsai -c conda-forge -c nvidia \
    libcuvs=25.10 cuda-toolkit=12.* python=3.11
"

# .env.gpu의 CONDA_ENV를 cuvs_dev_25로 변경 후 재빌드
```

`-Wl,-rpath`가 conda env path를 박아두므로 .so 재빌드 필요.

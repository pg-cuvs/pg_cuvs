# 2026-08-04 — 3O 상관 축 측정 (#88)

**결론: CAGRA BITSET prefilter(3O)의 recall 붕괴는 selectivity가 아니라 필터–쿼리 상관 방향이
지배한다.** anti-correlated 필터에서는 selectivity 0.0001부터 **0.5까지 전 구간 recall 0.0**이며,
같은 sel=0.5에서 비상관 필터는 0.989다. 즉 통과 집합의 **크기를 고정한 채 방향만 바꿔도** recall이
0↔1로 갈린다.

이는 새 실패 모드의 발견이 아니라 **알려진 실패 모드의 원인 변수 분리**다(§4 선행 연구 참조).

## 1. 방법

| 항목 | 값 |
|---|---|
| 데이터 | wiki_all_1M (1M × 768), `~/data/{corpus,queries_10k}.fbin` |
| 하드웨어 | A100-SXM4-80GB DGX (Brev), PG16, ext 0.5.0 |
| 쿼리 | 200 (확증 실험만 50), k=10 |
| 하네스 | `bench/filter_recall/adr079_3o_recall.py` (본 PR에서 상관 축 추가) |
| GT | numpy 독립 계산 — 필터 부분집합에 대한 exact top-k |

**상관 레벨 4종** (전부 **쿼리별** — 각 쿼리의 임베딩 거리 순서 기준):

- `spatial` — 그 쿼리에 **가장 가까운** sel 비율
- `mixed` — 절반 spatial + 절반 random
- `random` — 결정적 해시(`(id·2654435761) mod 10⁶`), 비상관. ADR-082와 동일 픽스처 = **회귀 앵커**
- `anti` — 그 쿼리에서 **가장 먼** sel 비율 (신규)

**경로 3종 강제**: 3O(`filter_auto_threshold=1.0`) / D-wedge(둘 다 0) /
stream_bf(`stream_bf_selectivity_threshold=1.0`). 실제 실행 경로는 `pg_stat_gpu_search`의
`search_mode`를 **쿼리마다** 읽어 기록하고, 균일하지 않으면 `MIXED:...`로 표기한다(조용한 폴백 탐지).

산출: `bench/results/adr079_3o_correlation{_anchor,,_hisel}.csv` (18 + 72 + 45 행).

## 2. 결과 — 3O recall

| selectivity | anti | random | mixed | spatial |
|---|---|---|---|---|
| 0.5 | **0.0** | 0.989 | — | 0.995 |
| 0.3 | **0.0** | 0.993 | — | 0.9985 |
| 0.1 | **0.0** | 0.996 | — | 1.0 |
| 0.05 | **0.0** | 0.997 | — | 1.0 |
| 0.02 | **0.0** | 0.9955 | — | 1.0 |
| 0.005 | **0.0** | 0.9945 | 1.0 | 1.0 |
| 0.002 | **0.0** | 0.9945 | 1.0 | 1.0 |
| 0.001 | 0.0005 | 0.9895 | 1.0 | 1.0 |
| 0.0005 | **0.0** | 0.8635 | 1.0 | 1.0 |
| 0.0002 | **0.0** | 0.47 | 1.0 | 1.0 |
| 0.0001 | 0.0005 | 0.282 | 1.0 | 1.0 |

모든 셀의 `daemon_search_mode`는 `cagra_prefilter` — 3O가 실제로 실행된 상태의 수치이며 조용한
폴백이 아니다.

### 확정 사항

1. **anti는 전 구간 붕괴한다.** selectivity를 4자리수(0.0001→0.5) 움직여도 recall 0.0이 변하지 않는다.
2. **selectivity는 예측 변수가 아니다.** 같은 `sel=0.02`에서 anti 0.0 / random 0.9955 / spatial 1.0.
   같은 `sel=0.5`(통과 50만 행)에서 anti 0.0 / random 0.989.
3. **`random`이 ADR-082를 재현한다**(0.9945@0.005 → 0.282@0.0001). 하네스·픽스처의 회귀 앵커.
4. **상관은 해로운 것이 아니라 방향이 문제다.** `spatial`/`mixed`는 비상관보다 **오히려 쉽다**
   (1e-4에서도 1.0). 이슈 #88의 원 가설("상관이 더 가혹")은 절반만 맞다.

기전상 자연스럽다 — 통과 행이 쿼리 근방에 뭉치면 그래프의 쿼리 주변 이웃이 통과 후보로 채워져
연결성이 보존되고, anti는 쿼리 근방에 통과 후보가 하나도 없어 순회가 통과 노드에 **도달조차 못
한다**.

## 3. 런타임 탐지 신호 — `mean_returned`

anti 실패는 "잘못된 이웃 반환"이 아니라 **"k개를 채우지 못함"**이다.

| 상태 | `mean_returned` (k=10) |
|---|---|
| anti (전 구간) | **0.03 ~ 2.02** |
| 건강한 모든 셀 | **정확히 10.00** |

분리가 넓고 예외가 없다. 필터 형상을 미리 알 필요 없이 "요청한 k보다 현저히 적게 반환됐다"를 폴백
트리거로 쓸 수 있다.

## 4. 선행 연구 — 무엇이 이미 알려져 있고 무엇이 우리 몫인가

### 이미 보고된 것

- **VecFlow** (arXiv 2506.00812, SIGMOD'26, cuVS 저자) — **CAGRA-Post/Inline 단일 글로벌 그래프 +
  필터**가 recall ~80% 천장에 걸리고 **매우 selective 필터에서 ~0%로 붕괴**함을 실측(WIKI-1M
  multi-label AND recall ~0). 원인을 **작은 교집합이 그래프 연결성을 파괴**하는 것으로 귀인.
  해법은 per-label 그래프(저카디널리티 known 범주형 컬럼 한정). ADR-079에 이미 기록돼 있다.
  → **"CAGRA가 필터에서 붕괴한다"는 우리 발견이 아니다.**
- **RACORN-1** (arXiv 2607.00768) — 우리와 **동일한 3레벨 상관 설계**(k-means 클러스터 기반
  positive/none/negative). ACORN-1이 음의 상관에서 붕괴: SIFT 1M 0.984→**0.161**, GIST 1M
  0.867→0.221, Text2Image 40M 0.651→**0.080**. §7.5에서 **"correlation is a determining variable
  on par with selectivity"**. 해법 ASF는 비통과 노드를 임시 다리로 허용해 끊긴 경로를 우회하며,
  **런타임 신호**(방문 노드 중 술어 통과 비율)로 전환한다. **CPU HNSW 계열이며 CAGRA는 명시적으로
  범위 제외**(§3.4).
- **VLDB 서베이** (*Filtered Vector Search: State-of-the-art and Research Opportunities*, vol.18) —
  필터가 "술어와 벡터 공간 사이의 상관을 유발"하며 무필터로 튜닝된 방법이 실패한다고 기술.
- **ACORN** (arXiv 2403.04871) — predicate-agnostic 필터 검색. 과밀 그래프로 필터 부분그래프의
  항해성을 보존.

### 우리 측정이 더하는 것

1. **원인 변수 분리.** VecFlow는 붕괴를 *작은 교집합*(≈낮은 selectivity)으로 귀인했고 RACORN-1은
   상관을 selectivity와 *동등한* 변수로 두었다. 우리는 **selectivity를 고정한 채 상관만 움직여**
   — sel=0.5, 통과 50만 행에서 anti 0.0 vs random 0.989 — CAGRA prefilter에서는 **상관이 지배하고
   selectivity는 거의 무관**함을 보인다. VecFlow의 multi-label AND는 두 요인이 섞여 있어 분리가
   불가능했다.
2. **블랙박스 탐지 신호.** RACORN-1의 신호는 순회 *내부*(방문 노드의 통과 비율)라 커널 수정을
   전제한다. `mean_returned ≪ k`는 **커널 밖에서** 관측되므로 cuVS를 수정하지 않는 우리 위치에
   적용 가능하다.
3. **exact 경로 무결성 확증**(§5) — 부수적.

### 함의: ACORN은 이 문제의 해법이 아니다

RACORN-1이 측정한 대상이 바로 **ACORN-1 자신의 붕괴**다. 따라서 "CAGRA에 ACORN 아이디어를 이식
(graph_degree 상향 = 과밀화)"하는 접근은 **음의 상관을 고치지 못할 가능성이 높다** — 문헌이 이미
그렇게 측정했다. 실제 처방은 RACORN-1의 ASF, 즉 **비통과 노드를 임시 다리로 허용해 끊긴 경로를
우회**하는 것이며, 이는 우리가 관측에서 추론한 실패 기전(순회가 비통과 영역을 지나지 못해 갇힘)과
정확히 일치한다. 두 경로가 독립적으로 같은 진단에 도달했다.

## 5. exact 경로의 sub-1.0은 동률 아티팩트다 (확증됨)

`anti` + 큰 후보집합에서 D-wedge/stream_bf가 recall 0.9885~0.9995로 1.0에 못 미쳤다. exact
경로이므로 설명이 필요했다.

**후보집합 크기 가설은 반증된다** — 같은 500k 후보에서 `random`·`spatial`은 정확히 1.0이고 `anti`만
어긋나며, anti 안에서 집합이 커질수록 악화된다(20k 0.9995 → 500k 0.9885).

**설명**: anti의 정답(=통과 집합 중 가장 가까운 것들)은 거리 분포의 먼 쪽에 놓이고, 집합이 커질수록
중앙값 부근 — 고차원에서 거리가 가장 조밀한 지점 — 으로 이동한다. 거리가 거의 동률이면 GPU 커널과
BLAS sgemm의 **합산 순서 차이**가 동률 원소의 선택을 바꾼다. TID 겹침 기반 recall은 그 교체를 오답
으로 센다.

**확증 실험** (`anti`, sel=0.5, 50쿼리, stream_bf 강제): 반환 top-10의 **거리**를 GT와 대조.

```
TID-recall = 0.9860    거리동등 쿼리 = 50/50    최대 거리차 = 9.53674e-07
```

9.54e-07 = 2⁻²⁰, float32의 **마지막 비트** 수준. **50개 쿼리 전부** 거리 동등 → exact 경로의
정확성에 결함이 없고 지표가 동률을 오답으로 셌을 뿐이다.

> **지표 방침**: exact 경로는 tie-robust(거리 기준)로 보고하고, TID 기준 수치를 병기할 때는 이
> 캐비엇을 함께 적는다.

**이 결과는 3O의 붕괴를 강화한다.** 두 실패가 다른 종류임이 확정됐다 — exact 경로는
`returned=10.00`으로 k를 다 채우고 거리도 GT와 같지만, 3O(anti)는 `returned=0.29~2.02`로 **k를
채우지도 못한다**. 3O의 recall 0.0은 동률 아티팩트가 아니라 실제 누락이다.

## 6. 한계

- **픽스처 강도가 문헌과 분리되지 않았다.** 우리 `anti`는 문자 그대로 "가장 먼 N개"로,
  RACORN-1(클러스터 불일치)이나 VecFlow(multi-label AND)보다 극단적이다. 우리가 **0.0**이고
  ACORN-1이 **0.08~0.22**인 차이가 *인덱스 구조 차이*(CAGRA vs HNSW+ACORN)인지 *픽스처 강도
  차이*인지 **분리되지 않았다.** 클러스터 기반 anti 픽스처를 추가하면 갈린다.
- **지연(p50)을 상관 축 간에 비교하면 안 된다.** `random`은 공유 필터 1개를 쓰지만
  `anti`/`spatial`/`mixed`는 쿼리마다 다른 `bigint[]`(sel=0.5면 50만 원소 ≈ 4MB)를 넘긴다.
  sel=0.5에서 spatial은 recall 1.0인데도 p50 947ms, random은 49.9ms — 이 차이는 검색이 아니라
  **필터 마샬링**이다. recall 비교만 유효하다.
- 같은 이유로 GT 계산도 상관 레벨에서 쿼리마다 반복된다(sel=0.5 기준 random 1.4초 vs anti 151초).
  이것이 측정 전체 wall-clock을 지배했다.
- 데이터셋 1종(wiki_all_1M, 768d)·k=10 고정. 차원·k 축 미측정.
- `anti`는 **최악 경계**다. 실제 SQL 술어가 거리 순위와 완벽히 역정렬되는 일은 드물다. 다만 표적
  세그먼트인 멀티테넌트에서 테넌트 데이터가 임베딩 공간에 분리돼 있으면 `WHERE tenant_id = ?`가
  구조적으로 이 방향이 될 수 있다. 라벨 필터는 상관-중립이 아니다 — 의미적 라벨은 보통 양의
  상관(=쉬운 쪽)이지만, 쿼리와 무관한 라벨을 걸면 음의 상관이 된다.
- 부수 관찰: 하네스가 CSV를 다 쓴 뒤에도 프로세스가 종료되지 않고 매달린다(재현 2회).

## 7. 재현

```
python bench/filter_recall/adr079_3o_recall.py \
  --data-dir ~/data --n 1000000 --queries 200 --k 10 --reuse-table \
  --correlations anti,random,mixed,spatial \
  --selectivities 0.005,0.002,0.001,0.0005,0.0002,0.0001 \
  --out bench/results/adr079_3o_correlation.csv
```

고-selectivity 보강은 `--correlations anti,random,spatial --selectivities 0.5,0.3,0.1,0.05,0.02`,
앵커는 `--correlations random`(기본 selectivity). 측정에 쓴 하네스가 본 브랜치 커밋과 바이트
일치함을 확인했다.

## 8. 파생

- **결정**: ADR-083 — selectivity 단일 변수 라우팅은 안전하지 않으며, 그래프 densification(ACORN)이
  아니라 런타임 탐지 + 폴백을 채택한다.
- **구현**: `mean_returned ≪ k` 탐지 + D-wedge 폴백, `cuvs.filter_auto_threshold` 문서 정정(별도 이슈).
- **ADR-079 정정**: VecFlow가 제기한 3O recall-ceiling 리스크는 확인되되 **귀인이 바뀐다** — 붕괴는
  실재하나 원인은 작은 교집합이 아니라 상관 방향이다(교집합이 커도 붕괴).
- **ADR-082 공백 해소**: "3O에 대해서는 상관 축을 재지 않았다"가 본 측정으로 닫힌다.
- **후속 후보**: 클러스터 기반 anti 픽스처(문헌과 강도 정렬), 차원·k 축, RACORN-1 ASF의 CAGRA
  적용 가능성 검토.

## 참고 문헌

- VecFlow — *A High-Performance Vector Data Management System for Filtered-Search on GPUs*,
  arXiv:2506.00812 (SIGMOD'26)
- RACORN-1 — *Adaptive Recall-Preserving Speedup for Low-Selectivity Filtered Vector Search*,
  arXiv:2607.00768
- ACORN — *Performant and Predicate-Agnostic Search Over Vector Embeddings and Structured Data*,
  arXiv:2403.04871 (SIGMOD'24)
- *Filtered Vector Search: State-of-the-art and Research Opportunities*, PVLDB vol.18

# 세션 보고서 — repo 공개 전 운영 하드닝 + 2-tier CI + 3O 비트셋 버그

| 항목 | 값 |
|------|-----|
| 기간 | 2026-06-09 ~ 2026-06-11 |
| 범위 | 사전 하드닝(#42–45) · 2-tier CI 구축(#46–48 + WIF) · CI가 잡은 3O 버그 수정(#49) |
| 결과 | 8 PR 머지. Tier-1 매 PR 자동·무료, Tier-2 버튼 1회 검증 완료. 출고 정확성 버그 1건 검출·수정 |
| 검증 | A100 `installcheck 26/26` + isolation `3/3`, Tier-1 CPU shim CI GREEN |

---

## 1. 요약 (TL;DR)

repo 공개를 앞두고 (1) 운영 하드닝(VRAM 예산·멀티테넌트 한계·관측성·OOM 내성)을 마치고,
(2) GPU 없이도 도는 **2-tier CI**를 구축했다. Tier-1은 GPU/CUDA를 CPU-reference shim으로
대체해 매 PR 무료로 plumbing·계약·정확성을 검증하고, Tier-2는 GitHub UI 버튼으로 실 A100
회귀를 on-demand 실행한다.

그 결과 **Tier-1 shim이 출고된 정확성 버그를 즉시 검출**했다 — 3O 필터의 BITSET 극성 반전.
TDD로 재현·수정하고, 회귀가드를 Tier-1에 상시 편입했다.

---

## 2. 배경 / 목표

- **GPU CI는 무료 옵션이 없다**(GitHub hosted 러너에 GPU 없음 → self-hosted = 유료 VM).
- 그러나 이번 프로젝트에서 실제로 무는 false-done 버그(3O rev map, manifest version,
  base_generation, MAX_INDEXES)는 **하나도 GPU 커널 버그가 아니라 glue**(IPC 직렬화, 데몬
  라우팅, fail-closed, mode 라벨링, manifest 계약)에서 났다.
- → 무는 버그 클래스는 **GPU 없이 잡힌다**는 가설. 이를 CI 전략(ADR-067)으로 정식화.

---

## 3. Part 1 — 사전 운영 하드닝 (#42–#45)

| PR | 내용 | 효과 |
|----|------|------|
| #42 `test(oom)` | extend-OOM poison 후 상주 CAGRA 그래프가 **재빌드 없이 생존**함을 검증 | OOM 내성 회귀가드 |
| #43 `feat(obs)` | `pg_stat_gpu_fallback` — per-index CPU-fallback 관측성 SRF | GPU→CPU 폴백 가시화 |
| #44 `feat(vram)` | 합리적 기본 VRAM 예산 + mempool-aware free (ADR-065) | 기본 총량 90% cap, 자기-회계 |
| #45 `feat(registry)` | `MAX_INDEXES` soft LRU cap — **65번째 테넌트 하드월 제거** | 멀티테넌트 진입장벽 해소 |

이 항목들은 뒤의 Tier-1 shim이 **결정적으로 재현**한다(shim의 fake VRAM 회계로 evict/budget/OOM
제어흐름을 실 GPU 없이 검증). 즉 하드닝과 CI가 맞물린다.

---

## 4. Part 2 — 2-tier CI 구축 (#46–#48 + WIF)

### 4.1 전략 (ADR-067, `design/CI_STRATEGY.md`)

| Tier | 머신 | 트리거 | 검증 대상 |
|------|------|--------|-----------|
| **1 — CPU-reference shim** | GitHub hosted (public repo = 무료) | **매 PR 자동** | plumbing·IPC 계약·fail-closed·mode 라벨링·정확성·VRAM 회계 로직 |
| **2 — 실 A100 installcheck** | 사용자 GPU VM (self-hosted) | **on-demand 버튼** | GPU 커널 correctness·approximate recall·실 VRAM 거동·latency |

### 4.2 Tier 1 — CPU-reference shim (#46)

- **shim 경계 = `src/cuvs_wrapper.h` 단일 헤더.** 데몬이 cuVS/CUDA를 호출하는 모든 지점이 이
  헤더 뒤에 있음(grep 검증: 비-wrapper TU의 cuda/cuvs는 주석뿐).
- `make PGCUVS_CPU_SHIM=1` → 실 `cuvs_wrapper.cu`(nvcc) 대신 `cuvs_wrapper_shim_cpu.c`(순수 C,
  35함수) 링크. `-lcuvs -lrmm -lcudart` drop → **CUDA 툴킷 0 의존, 순수 CPU 빌드.**
- shim 의미: build=host 사본, search=**metric별 exact kNN(=ground truth)**, serialize=실 round-trip,
  VRAM=결정적 fake 카운터. 데몬·백엔드 나머지 코드는 한 줄도 안 바뀜.
- `ci.yml`이 매 PR에 PG16+pgvector 설치 → shim 빌드 → 데몬 기동 → `installcheck-tier1` + isolation.

### 4.3 Tier 2 — UI 버튼 + WIF (#47, #48)

- **`workflow_dispatch`만**(Actions UI "Run workflow", 쓰기권한자 전용). 코멘트/라벨 자동
  트리거는 fork-PR가 VM에서 임의 코드 실행 위험이라 비채택.
- **3-job 모델**: `start-vm`(hosted, WIF 키리스 인증 → `gcloud start`) → `gpu-test`
  (VM 위 부팅-시작 self-hosted 러너가 실 `installcheck`) → `stop-vm`(`if: always()`, 비용 누수 방지).
- **#48**: WIF 미설정 시에도 동작(start/stop만 skip) → VM이 떠 있으면 버튼이 러너에서 바로 실행.

### 4.4 WIF 셋업 + end-to-end 검증

- 보안 모델: GCP 권한을 GitHub에 **키로 두지 않고**, GCP의 WIF 신뢰정책에 둠 —
  `attribute-condition=assertion.repository=='ysys143/pg_cuvs'`로 **이 repo 토큰만** 신뢰.
  SA 권한은 **이 VM 한 대 start/stop**으로 한정. 정적 키 0.
- 셋업: 사용자 프로젝트-admin gcloud(Cloud Shell)로 WIF pool/provider/SA/바인딩 생성 →
  변수 4개(`GCP_WIF_PROVIDER`, `GCP_CI_SA`, `GPU_CI_INSTANCE`, `GPU_CI_ZONE`) 설정.
- self-hosted 러너 `pg-cuvs-a100`(부팅-시작 systemd) 등록.
- **검증**: 버튼 dispatch → `Start VM` → A100 `installcheck 26/26` + isolation `3/3` → `Stop VM`,
  **3-job 전부 GREEN**, VM 자동 종료 확인. 완전 hands-off 동작 실증.

### 4.5 운영 결정 — 수동 토글 (현재 disabled)

버튼이 끝에서 VM을 auto-stop하는데, 떠 있던 dev VM이 그 부작용으로 꺼지는 게 싫어
**Tier-2(`gpu.yml`)를 `disabled_manually`로 전환**, VM은 수동(`make vm-start`/`vm-stop`) 제어.
재활성화는 `gh workflow enable "GPU regression (Tier 2)"` 한 줄(러너·WIF·변수 다 보존).
Tier-1(`ci.yml`)은 `active` 유지.

---

## 5. Part 3 — CI가 잡은 버그: 3O BITSET 극성 반전 (#49)

### 5.1 증상
`search_mode='cagra_prefilter'` 경로에서 테넌트 1로 필터한 kNN이 **제외 대상만** 반환
(`wrong_tenant=10/10`). 필터가 완전 반전. 기본 임계값에선 3O가 드물게 engage해 잘 안 드러남.

### 5.2 근본 원인 — 극성 규약 불일치

| 주체 | 규약 | 근거 |
|------|------|------|
| pg_cuvs 데몬 | `bit=1 = EXCLUDE` | `pg_cuvs_server.c:2541-2552` (`memset 0xFF` 후 유지 항목 클리어) |
| cuVS `bitset_filter` | `bit=1 = INCLUDE` | set된 비트를 유지 |
| 래퍼(버그) | 변환 없이 전달 | `cuvs_{cagra,bf}_search_filtered` 직통 |

오해의 발원지: 래퍼·헤더·shim·데몬 주석이 `bit=1=EXCLUDE`를 **"cuVS convention"**으로 오귀속.

### 5.3 수정
cuVS 경계에서만 비트셋 반전(`inv[w]=~bitset_words[w]`). 데몬·헤더·shim의 `bit=1=EXCLUDE`
계약은 불변 — cuVS 어댑터만 번역. `cuvs_cagra_compact`의 `keep_bits`(cuVS-native)는 무관.
주석 오귀속 교정.

### 5.4 검증 (TDD)
1. **RED**: 골든을 정답 `0`으로 먼저 수정 → 버그난 A100에서 `10≠0` 단일행 diff로 실패 확인.
2. **GREEN**: 반전 적용 → A100 `26/26` + isolation `3/3`, `filter_comparison` `wrong_tenant=0`.
3. `filter_comparison`을 **Tier-1 상시 가드로 복귀** → CPU shim CI GREEN(shim=골든=0).

---

## 6. 교훈 — CI 전략의 자기 검증

이 버그는 **GPU 자기-검증만으론 영원히 못 잡는다.** 실 A100은 발견 전에도 `26/26` 통과했다 —
골든이 깨진 출력(`wrong_tenant=10`)을 *축복*하고 있었기 때문(false-done 골든). GPU가 채점
기준이면 GPU의 오류는 자기 자신과 늘 일치한다.

**Tier-1 CPU shim**은 문서 계약을 *정확히* 구현해 독립적인 정답(`0`)을 냈고, GPU 골든(`10`)과의
불일치로 버그를 첫 실행에 노출했다.

> 핵심: 가속기 경로의 정확성은 **독립적인 정답 기준**과 대조해야 검증된다. 자기-검증은
> false-done을 통과시킨다. 이번 수정으로 회귀가드가 매 PR Tier-1에 편입돼 재발은 자동 차단.

---

## 7. 산출물 / 남은 것

### 머지된 PR (이번 세션)
| PR | 제목 |
|----|------|
| #42 | test(oom): resident CAGRA graph survives extend-OOM poison without rebuild |
| #43 | feat(obs): pg_stat_gpu_fallback — per-index CPU-fallback observability |
| #44 | feat(vram): sane default VRAM budget + mempool-aware free (ADR-065) |
| #45 | feat(registry): MAX_INDEXES soft LRU cap — remove the 65th-tenant hard wall |
| #46 | feat(ci): Tier 1 CPU-reference shim — GPU-less build + installcheck (ADR-067) |
| #47 | feat(ci): Tier 2 — on-demand GPU regression via UI button + WIF |
| #48 | ci(gpu): run Tier 2 without WIF when the VM is already up |
| #49 | fix(3o): invert prefilter BITSET for cuVS polarity (bit=1=include) |

### 현재 CI 상태
- **Tier-1** (`ci.yml`): `active` — 매 PR 자동·무료, CPU shim, `filter_comparison` 포함.
- **Tier-2** (`gpu.yml`): `disabled_manually` — 수동 VM 제어 위해 off. 재활성화 1줄.

### 남은 것 (선택)
- WIF 자동 start/stop은 검증 완료됐으나 운영상 Tier-2를 수동 토글로 둠. 필요 시 enable.
- 정직성 라벨(README): "CI(Tier 1)=CPU reference. GPU 커널·approximate recall·실 VRAM은
  on-demand A100(Tier 2)에서 검증" 명시 — green badge의 false-done 방지.

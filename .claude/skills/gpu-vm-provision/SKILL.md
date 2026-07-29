---
name: gpu-vm-provision
description: >
  pg_cuvs GPU VM 프로비저닝/재빌드 전문 스킬. 현 메인 프로바이더는 Brev(A100급)이며,
  신규 인스턴스를 bootstrap.sh로 zero-to-ready(~10분)까지 구성하는 워크플로우를 다룬다.
  사양 요구는 용도에 따라 갈린다 — 벤치마크는 고정 사양, 개발·회귀는 A100급이면 된다.
  GCP Terraform 경로는 레거시로 유지된다.

  트리거 키워드: "VM 올려줘"(Brev에선 생성+부트스트랩을 뜻함), "VM 프로비저닝",
  "VM 새로 만들자", "GPU VM 생성", "brev",
  "bootstrap", "재빌드", "fresh provisioning", "libcuvs 설치", "cuVS 환경 구성",
  "GPU dev 환경 셋업", "VM 망가짐", "VM 복구", "ldconfig 문제",
  "libstdc++ GLIBCXX", "terraform apply"(레거시 GCP) 등이 나오면 사용.

  포함 시나리오: (1) Brev에 새 인스턴스 생성 + 부트스트랩, (2) 인스턴스 삭제 후
  재빌드, (3) 부트스트랩/빌드 중 conda+CUDA+PG 통합 함정 대응,
  (4) cuVS 25.x/26.x API 변경 대응, (5) 레거시 GCP Terraform 프로비저닝.
  단, 이미 프로비저닝된 VM에서의 일상 빌드 루프(make sync/gpu-build/gpu-test)는
  pg-cuvs-dev 스킬을 사용하라.
---

# gpu-vm-provision

pg_cuvs 개발용 GPU VM의 프로비저닝/재빌드/검증을 다룬다. 시행착오로 검증된 패턴만 포함.
프로바이더 현황(main=Brev, available=GCP, historical=RunPod)은 `infra/README.md` 참조.

## Brev 모델 이해 (가장 중요)

Brev 인스턴스에는 사양과 무관하게 **"보존"이라는 개념이 없다**:
- `brev stop` 미지원, 영속 볼륨 없음, 스냅샷/이미지 기능 없음
- 과금을 멈추는 유일한 방법은 `brev delete` — 인스턴스의 모든 것이 사라진다
- 따라서 전략은 보존이 아니라 **빠른 재현 가능한 재빌드**다: bootstrap.sh가
  fresh instance → ready(확장 빌드 + 데몬 + PG16 preload + wiki_all_1M 데이터셋)를
  ~10분에 완료한다 (setup ~80s, dataset ~75s, build ~2-3분, verify ~30s)

VM 위의 어떤 것도 source of truth가 아니다. 소스는 `main` 브랜치에, 측정 CSV는
`bench/results/`에 커밋되어 있으므로 delete의 비용은 ~10분 재빌드뿐이다.

## 부트스트랩 위치

부트스트랩은 리포에 커밋되어 있다. 접속 정보(IP·project id)만 사설 문서 레포
`pg_cuvs_docs/vm-access/VM_ACCESS.md`에 남는다:

```
infra/brev/
├── bootstrap.sh   # zero-to-ready 스크립트, 모든 함정이 주석과 함께 박혀 있음
└── README.md      # 재시작 절차 + gotcha 7종 상세
```

## 재빌드 절차

**사양 요구는 용도에 따라 다르다.** 예전에는 모든 작업에 한 사양을 고정했는데,
그 경직성이 실제로 비용을 냈다 — 2026-07-29 고정 사양 재고 소진 때 벤치마크가
아닌 회귀 검증까지 멈춰 세웠다. 실제로 고정이 필요한 것은 벤치마크뿐이다.

| 용도 | 요구사항 |
|------|----------|
| **벤치마크 측정** | `massedcompute_A100_sxm4_80G` **고정**. canonical 수치가 이 머신에서 나왔다(`BENCHMARK.md` §2.1b, `bench/results/README.md`) — 절대값을 기존 결과와 비교하려면 같은 머신이어야 한다. 재고가 없으면 **기다린다**. |
| **개발·회귀 검증** | CUDA 되는 **A100급**이면 충분. 아래 세 조건만 맞추면 된다. |

개발용으로 다른 인스턴스를 쓸 때 확인할 것:

1. **`CUDA_ARCH`를 GPU에 맞춘다** — A100=sm_80, L4=sm_89, H100=sm_90 (`gpu.conf`).
2. **이미지 계열** — 부트스트랩은 `shadeform` 유저 + systemd를 전제한다. 다른 계열
   (RunPod: root, systemd 없음)이면 `USER_HOME`과 `systemctl`→`pg_ctlcluster` 교체 필요.
3. **`routing_golden_measured`는 이식 불가** — 테스트 헤더가 직접 밝히듯 실측 계수로
   cagra-vs-seqscan 교차점을 판정하므로 GPU 모델이 바뀌면 재기준선이 필요할 수 있다.
   A100 계열끼리는 통과 확인됨(massedcompute ↔ paperspace, 2026-07-29).

검증된 대체(A100-SXM4-80GB, 환경 동일): `paperspace_A100_sxm4_80G` ($3.936/hr,
고정 사양의 2.4배). **개발·회귀 전용 — 벤치마크 수치를 여기서 뽑지 마라.**

여전히 유효한 금지: 위 표에 없는 사양을 `brev search`로 훑어 즉석에서 고르지 마라.
필요하면 사용자에게 확인하고, 쓰기로 정했다면 이 표에 추가한다.

1번은 에이전트가 생성할 수 있다. 다만 과금이 시작되는 행위이므로, 생성 직전에 사양과
시간당 단가를 제시하고 **사용자 확인을 받은 뒤** 실행한다.

```bash
# 1. 인스턴스 준비 — 신규 생성이면 사용자 확인 후 에이전트가 생성, 기존 인스턴스면 재시작
brev create <name> --type massedcompute_A100_sxm4_80G   # 벤치마크용 (확인 후)
brev create <name> --type paperspace_A100_sxm4_80G      # 개발·회귀용 대체 (확인 후)
brev start <name>                                        # 기존 인스턴스 재시작
brev refresh                 # ~/.brev/ssh_config 갱신 → `ssh <name>` 가능해짐

# 2. 부트스트랩 전송 + 실행 (~10분)
scp infra/brev/bootstrap.sh <name>:/home/shadeform/
ssh <name> 'bash /home/shadeform/bootstrap.sh 2>&1 | tee bootstrap.log'

# 3. 완료 후 로컬 gpu.conf의 호스트를 새 인스턴스로 갱신 → pg-cuvs-dev 스킬로 전환
```

환경 상수 (shadeform 계열 이미지 — massedcompute·paperspace 모두 동일, 2026-07 기준):
Ubuntu 22.04, 유저 `shadeform`(passwordless sudo, systemd 있음), conda는
`/opt/miniforge3`(`cuvs_dev` 빌드 env + `cuvs_bench` python env 분리), A100이면
CUDA_ARCH=sm_80. 부트스트랩은 이 상수들에 의존하므로, 이미지 계열이 다르면
위 "이미지 계열" 항목대로 수정이 필요하다.

## 빌드 함정 (bootstrap.sh에 이미 박혀 있음)

새로 디버깅하지 말 것 — 아래는 색인이고, 상세와 해법 원라이너는
`infra/brev/README.md`의 gotcha 목록과 bootstrap.sh 주석에 있다:

1. `ld: cannot find -lstdc++` — gcc=12/g++=11 불일치 → `libstdc++-12-dev` 설치
2. `nvcc fatal: cannot execute cc1plus` — `make NVCC="nvcc -ccbin /usr/bin/g++"`
3. PG environment 파일은 값을 **quote** 해야 재시작됨
4. `cuvs.*` GUC는 shared_preload_libraries 설정 + 재시작 **후에만** 인식
5. conda libpq의 unix socket 경로 불일치 → `export PGHOST=/var/run/postgresql`
6. bench는 psycopg(v3)+pgvector 필요 → 별도 `cuvs_bench` env
7. conda를 `/opt`에 두고 `chmod o+rX` (postgres 유저 접근). **conda lib 디렉터리를
   ldconfig에 등록 금지** (sshd의 OpenSSL이 깨져 VM 접근 불가) —
   `libstdc++.so.6`/`libgcc_s.so.1`만 `/usr/local/lib`에 심볼릭 링크

프로바이더가 다르면(예: RunPod — root 유저, systemd 없음): USER_HOME 변경,
`systemctl` → `pg_ctlcluster 16 main restart`로 교체.

## 레거시: GCP Terraform 경로

GCP 인스턴스(stop/start 모델, ephemeral IP)는 여전히 사용 가능하지만 메인이 아니다:

| 상황 | 문서 |
|------|------|
| GCP 신규 프로비저닝 | `references/quick-start.md` (Terraform, `infra/gcp/`) |
| GCP VM 망가져 SSH 불가 | `references/failures/disk-recovery.md` (rescue VM + chroot) |
| GCP 빌드/install 실패 | `references/troubleshooting.md` |
| GCP 생애주기/복구 운영 | `docs/playbooks/gpu-vm-lifecycle.md` (GCP 전용 플레이북) |
| cuVS API 변경/SIGSEGV | `references/cuvs-26x-quirks.md` (프로바이더 무관) |

Brev에는 디스크 레스큐 개념이 없다 — 망가지면 `brev delete` 후 재부트스트랩이 복구다.

## 비용 원칙

세션 종료 시 반드시 `brev delete` (stop이 없으므로 delete만이 과금 중지).
다음 세션 시작 비용은 ~10분 재빌드다. 레거시 GCP 인스턴스는 `make vm-stop`.

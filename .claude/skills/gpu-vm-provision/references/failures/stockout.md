# STOCKOUT — GPU 자원 부족

## 증상

```
Error 503: ZONE_RESOURCE_POOL_EXHAUSTED_WITH_DETAILS
  NULL:0/NULL:0/NULL:0 (state:STOCKOUT, sub-state:STOCKOUT, resource type:compute)
```

`terraform apply` 또는 `gcloud compute instances start` 시 발생.

## 원인

해당 zone에 GPU(A100, L4, H100) 인스턴스 capacity가 일시적으로 없음. quota는 있어도 실제 물리 자원이 없으면 거부됨.

## 해결

다른 zone으로 변경. A100 가용 zone(2026년 5월 기준):

```bash
# 가용 zone 확인
gcloud compute accelerator-types list --filter="name:nvidia-tesla-a100" \
  --format="value(zone)" | sort -u
```

대표 zone:
- us-central1-a, us-central1-b, us-central1-c, us-central1-f
- us-east4-c
- asia-east1-a, asia-east1-b, asia-east1-c

`infra/gcp/terraform.tfvars` 수정:
```hcl
# zone = "us-central1-a"  # STOCKOUT
zone = "us-central1-b"    # 시도
```

이후 `terraform apply` 재실행.

## 예방

- 작업 마치면 `make vm-stop`이 아니라 `terraform destroy`로 자원 반납
- 인기 시간대(US 업무시간) 피하면 STOCKOUT 빈도 감소
- 정 안 되면 region 자체를 바꿔도 됨 (us-east4, europe-west4 등)

## 참고

STOCKOUT은 시간이 지나면 풀린다. 5-10분 후 재시도해도 됨. 다만 같은 zone에 계속 매여 있을 필요 없으니 보통 zone 변경이 빠름.

---

## Brev (현 메인 프로바이더)

위 절차는 **GCP 전용**이다. Brev 에는 zone 개념이 없어 그대로 쓸 수 없다.

### 증상

```
Type massedcompute_A100_sxm4_80G had failures, trying next type...
Warning: Only created 0/1 instances
```

`brev search` 결과에서 해당 type 이 **아예 사라지는** 경우도 있다(공급자 capacity 소진).

### 대응

**먼저 용도를 가려라** (`SKILL.md` 재빌드 절차의 표):

- **벤치마크 측정이면 기다린다.** canonical 수치가 `massedcompute_A100_sxm4_80G` 에서 나왔으므로(`BENCHMARK.md` §2.1b) 다른 머신의 절대값은 기존 결과와 비교할 수 없다. 재고 복귀를 기다리거나, 그동안 VM 없이 가능한 작업(로컬 `make test-unit`, 정적 분석, 문서)을 한다.
- **개발·회귀 검증이면 대체해도 된다.** 검증된 대체는 `paperspace_A100_sxm4_80G` ($3.936/hr, shadeform 계열이라 환경 동일 — 부트스트랩 무수정 동작 확인, 2026-07-29). 과금이 고정 사양의 2.4배이므로 생성 전 단가를 제시하고 사용자 확인을 받는다.

재시도는 해볼 만하다 — 일시적 품절이면 수 분~수십 분 내 복귀하는 경우가 있다. 다만 `brev search` 목록에서 아예 사라졌다면 대기가 길어질 수 있다.

**표에 없는 사양을 즉석에서 고르지 마라.** 위 둘로 안 되면 사용자에게 확인하고, 새로 쓰기로 정했다면 `SKILL.md` 의 표에 **용도와 함께** 추가한다 — 그래야 다음 세션이 같은 조사를 반복하지 않는다.

미검증 동일 계열 후보(참고용, 사용자 승인 시에만): `denvr_A100_sxm4`, `gpu_1x_a100_sxm4`. 둘 다 40GB VRAM 이라 `paperspace`(80GB)보다 원본과 멀다.

### 확인 명령

```bash
brev search --min-vcpu 8 --json | python3 -c "
import sys,json
d=json.load(sys.stdin); rows=d if isinstance(d,list) else d.get('instances') or []
m=[r for r in rows if r.get('type')=='massedcompute_A100_sxm4_80G']
print('가용' if m else '미가용', m[0].get('price_per_hour') if m else '')
"
```

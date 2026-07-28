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

**다른 사양으로 대체하지 마라.** 사양 고정이 이 스킬의 전제다 — 벤치마크·회귀 검증은 머신이 같아야 결과를 비교할 수 있다(`SKILL.md` 재빌드 절차의 금지 조항 참조).

1. 재시도한다. 일시적 품절이면 수 분~수십 분 내 복귀하는 경우가 있다.
2. 복귀를 기다리는 동안 VM 없이 가능한 작업(로컬 `make test-unit`, 정적 분석, 문서)을 진행한다.
3. 장시간 미복귀로 대체가 불가피하면 **사용자에게 확인**하고, 대체 사양을 쓰기로 정했다면 그 값을 `SKILL.md` 의 고정 사양으로 함께 갱신한다. 임의 선택 후 스킬을 그대로 두면 다음 세션이 또 다른 사양을 고르게 된다.

동일 계열 후보(참고용 — 임의 사용 금지, 사용자 승인 시에만):
`denvr_A100_sxm4`, `paperspace_A100_sxm4_80G`, `gpu_1x_a100_sxm4`.

### 확인 명령

```bash
brev search --min-vcpu 8 --json | python3 -c "
import sys,json
d=json.load(sys.stdin); rows=d if isinstance(d,list) else d.get('instances') or []
m=[r for r in rows if r.get('type')=='massedcompute_A100_sxm4_80G']
print('가용' if m else '미가용', m[0].get('price_per_hour') if m else '')
"
```

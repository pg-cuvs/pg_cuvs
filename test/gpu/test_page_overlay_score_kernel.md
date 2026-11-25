# 대기 행 CUDA 채점 커널 단위 시험

배정된 Linux CUDA 서버에서 저장소 루트 기준으로 실행한다. 시험은 `cuvs_page_overlay_launch_scores`에 한 질의와 packed VECTOR 행을 넘긴다. `cudaMallocHost`로 만든 출력 버퍼에 비기본 스트림으로 D2H 복사하고, 스트림 동기화 후 결과를 읽는다. 출력 앞뒤 8개 float에는 canary를 둔다. 출력 본문도 사전에 `+Inf`로 채워 누락된 쓰기를 검출한다.

호스트 오라클은 `8673c9c3`의 순서대로 각 좌표의 double 값을 누산하고 float로 한 번 반올림한다. 반환된 유한 점수는 부호 있는 0까지 32비트 전체가 같아야 한다. 비유한 입력이나 float로 변환한 기준 점수가 비유한 행은 quiet NaN을 요구한다. 유한 입력에도 커널이 증명하지 못하면 quiet NaN을 반환할 수 있다. 이 경우 가려진 점수의 기존 CPU 재계산은 데몬 통합 시험 범위다.

| 시험 인자 | 검사 |
| --- | --- |
| `normal` | 768D xorshift32 행 257개 × 고정 질의 4개 × L2·IP·cosine. 각 질의·거리에서 최소 232개의 비트 일치 점수를 요구한다. |
| `edge` | 빈 호출과 잘못된 인자, 짧고 홀수인 차원, partial warp/block, 동점과 반올림 midpoint·인접 ULP, subnormal 입출력과 ±0, overflow, NaN/Inf, cosine zero norm을 검사한다. |
| `guard` | 768D에서 큰 항 두 개와 1을 다른 좌표에 넣어, 순서대로 더한 IP와 warp 분할 합계가 달라지는 입력을 검사한다. 같은 입력으로 cosine 결과도 대조한다. |
| `all` | 기본값. 위 시험 전체를 실행한다. |

## 정상판 실행

제품 헤더를 임시 위치에 복사해 동일한 입력 스냅샷에서 정상판과 변이판을 빌드한다. `CUDA_ARCH` 기본값은 A100용 `sm_80`이다. 호스트 오라클의 연산 결합을 끄고, device 컴파일은 일반 CUDA 산술 설정을 사용한다.

```bash
set -euo pipefail
OUT=$(mktemp -d)
NVCC_BIN=${NVCC:-nvcc}
flags=(-std=c++17 -O3 "-arch=${CUDA_ARCH:-sm_80}" -Xcompiler=-ffp-contract=off)
test_src=test/gpu/test_page_overlay_score_kernel.cu
header=cuvs_page_overlay_gpu_kernel.cuh
mkdir "$OUT/original" "$OUT/mutant"
cp "src/$header" "$OUT/original/$header"
sha256sum "$test_src" "$OUT/original/$header"
"$NVCC_BIN" "${flags[@]}" -I"$OUT/original" -Isrc "$test_src" -o "$OUT/kernel-test"
"$OUT/kernel-test" all
```

## 오차 경계 제거 변이

정상판이 통과한 같은 셸에서 실행한다. IP·L2의 interval 반폭을 0으로 바꾸되 나머지 판정은 유지한다. IP 반례 중 좌표 0·1·32에 `2^60`, `1`, `-2^60`을 넣고 질의를 모두 1로 두면, ordered double에서는 1이 반올림으로 사라져 합이 `+0`이다. 현재 커널에서는 lane 0이 두 큰 항을 먼저 상쇄하고 lane 1의 `1`을 더한다. 이 warp 합계에 반폭 0을 적용하면 잘못된 `1.0f`가 증명된 점수로 반환된다.

```bash
python3 - "$OUT/original/$header" "$OUT/mutant/$header" <<'PY'
from pathlib import Path
import sys

original, mutant = map(Path, sys.argv[1:])
source = original.read_text()
anchor = 'return round_interval(sum, __dmul_ru(4.0 * r, mass));'
if source.count(anchor) != 1:
    raise SystemExit('expected exactly one IP/L2 round_interval anchor')
mutant.write_text(source.replace(anchor, 'return round_interval(sum, 0.0);'))
PY
"$NVCC_BIN" "${flags[@]}" -I"$OUT/mutant" -Isrc "$test_src" -o "$OUT/kernel-mutant"
rc=0
"$OUT/kernel-mutant" guard >"$OUT/mutant.log" 2>&1 || rc=$?
cat "$OUT/mutant.log"
[[ $rc -eq 1 ]]
grep -F 'score mismatch case=guard metric=ip ' "$OUT/mutant.log"
```

RED는 변이판 컴파일이 통과하고 `guard`의 IP 비트 불일치로 종료 코드 1이 나온 경우다. CUDA 런타임 오류는 종료 코드 2로 구분한다.

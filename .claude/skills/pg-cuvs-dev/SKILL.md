---
name: pg-cuvs-dev
description: >
  pg_cuvs 일상 GPU 개발 워크플로우 전문 스킬. Brev A100 VM에서의
  Makefile gpu-* 루프(sync → gpu-build → gpu-install → gpu-test), gpu.conf 설정,
  빌드/테스트 중 CUDA/cuVS/PG16 함정 대응을 다룬다.
  "gpu-build 안 돼", "conda 환경 활성화", "nvcc 에러", "make gpu-", "gpu.conf",
  "installcheck 실패", "CUDA 경로", "sync 안 됨", "VM 접속",
  "VM 내려줘", "VM 삭제", "인스턴스 정리"(세션 종료 = brev delete) 키워드가
  나오면 이 스킬을 사용하라.
  단, 인스턴스를 새로 만들거나(brev + bootstrap.sh) 재빌드하는 작업, VM이 망가진
  경우, libcuvs/CUDA 환경 최초 구성은 gpu-vm-provision 스킬을 사용하라.
---

# pg_cuvs GPU 개발 환경 가이드

Brev A100 VM 기반 개발 환경. 로컬 Mac은 코드 편집만, 빌드/테스트는 VM에서 실행 (ADR-004).

## 전체 흐름

```
로컬 Mac (코드 편집)
  │  make sync
  ▼
Brev A100 VM (shadeform@<host>, ~/pg_cuvs/)
  │  conda activate cuvs_dev   (/opt/miniforge3)
  │  make  →  sudo make install  →  make installcheck
  ▼
PostgreSQL 16 (VM 로컬)
  │  CREATE EXTENSION pg_cuvs
  ▼
GPU (NVIDIA A100-SXM4-80GB, sm_80, 80GB VRAM)
```

---

## 1. VM 준비

**인스턴스 신규 생성·부트스트랩·재빌드는 이 스킬의 범위가 아니다** —
`gpu-vm-provision` 스킬을 따르라 (Brev는 stop이 없어 "재시작 = 재부트스트랩 ~10분").
여기서는 이미 부트스트랩된 인스턴스를 쓰는 일상 루프만 다룬다.

### SSH 연결

```bash
brev refresh            # ~/.brev/ssh_config 갱신 → `ssh <name>` 동작
ssh <name>              # 예: ssh slimy-indigo-bison (유저 shadeform)
```

### gpu.conf 설정

Makefile의 `gpu-*` 타깃은 `gpu.conf`(gitignored)를 읽는다. Brev에는 gcloud IP
조회가 없으므로 `VM_SSH_HOST`가 그대로 접속 호스트가 된다:

```bash
# gpu.conf (gitignored, 비밀 없음)
VM_SSH_HOST=<brev-host-alias>   # 예: pg-cuvs-verify (brev refresh 가 ssh_config 에 등록)
CONDA_ENV=cuvs_dev
CUDA_ARCH=sm_80                 # A100=sm_80 (L4=sm_89, H100=sm_90)
```

파일명이 예전엔 `.env.gpu` 였다. 비밀이 없는데도 `.env` 접두사 때문에 dotenv 읽기를
거부하는 도구에 걸려서 `gpu.conf` 로 바꿨다. 레거시 이름도 계속 읽히고
`VM_SSH_HOST` 는 옛 `GCP_VM` 으로 폴백하므로, 기존 설정을 안 고쳐도 동작한다.

주의: `make vm-start` / `make vm-stop` / `make vm-ip`는 gcloud 전용 타깃이라
Brev에서는 동작하지 않는다. Brev에서 과금을 멈추려면 `brev delete`뿐이다 —
gpu-vm-provision 스킬 참조.

### 데몬 제어

부트스트랩이 `pg-cuvs-server` systemd 유닛을 설치하므로 데몬은 systemd로 다룬다
(예전엔 `nohup`이라 이 명령들이 존재하지 않았고, 세션이 끊기면 데몬도 죽었다):

```bash
ssh <name> "sudo systemctl restart pg-cuvs-server"     # 재기동
ssh <name> "systemctl is-active pg-cuvs-server"        # 상태
ssh <name> "sudo journalctl -u pg-cuvs-server -n 50 --no-pager"   # 로그
```

`restart` 후 소켓은 **즉시 생기지 않는다** — 첫 CUDA 컨텍스트가 필요해서 머신에
따라 12초~3분 걸린다. 유닛의 `TimeoutStartSec=600`이 이걸 감안한 값이고, 그동안
`activating` 상태로 머무는 것이 정상이다. 소켓 권한(0660 → 0666) 확장은
`ExecStartPost`가 자동으로 하므로 손으로 `chmod` 할 필요가 없다.

---

## 2. 일상 워크플로우

| 명령 | 용도 |
|------|------|
| `make sync` | 로컬 → VM rsync (.o/.so/gpu.conf 제외) |
| `make gpu-build` | VM에서 make (nvcc + PGXS) |
| `make gpu-install` | VM에서 sudo make install |
| `make gpu-test` | VM에서 make installcheck |
| `make gpu-shell` | VM SSH 대화형 세션 |
| `make gpu-e2e` | sync → build → install → test 전체 |

세션 종료 시: 커밋/푸시 확인 후 `brev delete` (VM 위 자산은 모두 재빌드 가능해야 함 —
소스는 main에, 측정 CSV는 `bench/results/`에 커밋).

---

## 3. 자주 겪는 함정

### A. nvcc가 libcuvs 헤더를 못 찾음
**증상**: `fatal error: cuvs/neighbors/cagra.h: No such file or directory`
**원인**: conda 환경이 활성화되지 않은 상태에서 빌드
**해결**: `gpu-build` 타깃이 conda activate를 수행하는지 확인
```bash
ssh <name> "source /opt/miniforge3/bin/activate cuvs_dev && echo \$CONDA_PREFIX"
# /opt/miniforge3/envs/cuvs_dev 출력되면 정상
```

### B. nvcc가 cc1plus를 실행 못 함
**증상**: `nvcc fatal: cannot execute cc1plus`
**원인**: conda의 nvcc가 자기 툴체인의 cc1plus를 못 찾음
**해결**: `make NVCC="nvcc -ccbin /usr/bin/g++"` (bootstrap.sh가 인코딩한 gotcha #2)

### C. PGXS가 PostgreSQL 헤더를 못 찾음
**증상**: `pg_config: command not found`
**해결**:
```bash
ssh <name> "which pg_config && pg_config --version"
# PostgreSQL 16.x 출력되면 정상
```

### D. nvcc와 PG 헤더의 float4 충돌
**증상**: `error: redefinition of 'float4'`
**원인**: .cu 파일에서 PG 헤더를 직접 include한 경우
**해결**: ADR-001 참조. `.c`/`.cu` 분리, `cuvs_wrapper.h`의 `extern "C"` 인터페이스만 공유

### E. postmaster가 libcuvs를 찾지 못함
**증상**: `CREATE EXTENSION pg_cuvs` 시 `ERROR: could not load library ... libcuvs.so`
**해결**: `SHLIB_LINK`의 `-Wl,-rpath,$(CUVS_LIB)` 확인 (ADR-007). conda가 `/opt`에
있고 `chmod o+rX` 되어 있어야 postgres 유저가 접근 가능 (bootstrap gotcha #7)

### F. installcheck 실패 — postmaster 연결 불가
**증상**: `pg_regress: could not connect to the postmaster`
**해결**:
```bash
ssh <name> "sudo systemctl status postgresql@16-main"
ssh <name> "sudo systemctl restart postgresql@16-main"
```
벤치 스크립트는 `export PGHOST=/var/run/postgresql` 필요 (conda libpq socket 경로 불일치, bootstrap gotcha #5)

### G. sync 후 VM의 .o 파일이 로컬 빌드 결과물로 덮어써짐
**증상**: `make`는 성공하나 nvcc .o가 아닌 로컬 gcc .o가 링크됨
**해결**: Makefile sync 타깃의 `--exclude 'src/*.o'` 확인

### H. 절대 하지 말 것: conda lib을 ldconfig에 등록
```bash
# 이렇게 하면 안 됨
echo "$CONDA_PREFIX/lib" | sudo tee /etc/ld.so.conf.d/cuvs.conf && sudo ldconfig
```
conda env lib에는 `libssl.so.3`, `libdbus-1.so.3` 같은 시스템 라이브러리가 섞여 있어
ldconfig 등록 시 sshd/dbus가 ABI 충돌로 죽고 VM 접근 불가가 된다.
**올바른 방법**: `-Wl,-rpath` + `libstdc++.so.6`/`libgcc_s.so.1`만 `/usr/local/lib`에
심볼릭 링크 (bootstrap.sh가 이미 수행, gotcha #7 — 해법이 바뀌면 그쪽이 SSoT).
**복구**: Brev에는 디스크 레스큐가 없다 — `brev delete` 후 재부트스트랩
(gpu-vm-provision 스킬). 레거시 GCP 인스턴스만 disk-recovery 절차 적용.

---

## 4. 비용 관리

Brev는 stop이 없다 — 인스턴스가 떠 있는 동안 계속 과금된다.

- 작업을 마치면 커밋/푸시 후 `brev delete`
- 다음 세션 시작 비용은 재부트스트랩 ~10분 (gpu-vm-provision 스킬)
- VM에만 존재하는 산출물을 만들지 말 것 — 측정 결과는 즉시 `bench/results/`로 커밋

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| `infra/README.md` | 프로바이더 현황 (Brev main / GCP legacy / RunPod historical) |
| `infra/brev/` | bootstrap.sh + gotcha 상세 |
| `design/decisions.md` ADR-004 | 원격 GPU VM 개발 모델 |
| `design/decisions.md` ADR-001 | C/.cu 분리, float4 충돌 |
| `design/decisions.md` ADR-007 | -Wl,-rpath 이슈 |
| `docs/playbooks/gpu-vm-lifecycle.md` | GCP 전용 생애주기 플레이북 (레거시) |

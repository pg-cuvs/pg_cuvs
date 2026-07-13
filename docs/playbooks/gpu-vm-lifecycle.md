# Playbook: GPU VM 생애주기 및 복구 (provisioning / stop-start / recovery)

pg_cuvs의 GPU VM(개발용 `pg-cuvs-dev` 1×A100, 멀티 GPU 수용 `pg-cuvs-dev-mgpu`
2×A100)을 start/stop/reset 할 때 발생하는 운영 함정과 복구 절차. **둘 다 GCP
project `your-gcp-project`에 있다** — gcloud 명령에 항상
`--project your-gcp-project`를 붙인다(기본 project가 다름).

---

## 1. 증상 (Symptoms)

- VM stop/start 후 `make sync`/`ssh`가 **이전 IP로 접속 실패**(timeout/no route).
- VM stop/start 후 데몬이 `[ERROR] pg_cuvs_server: no CUDA GPUs detected`로 죽는다.
- `nvidia-smi`가 `Failed to initialize NVML: Driver/library version mismatch`.
- 재시작 후 `/tmp/cuvs_indexes`가 비어 있다(인덱스 artifact가 사라짐).
- GCS snapshot이 "동작 안 함"(업로드/다운로드 실패) — VM에 service account 없음.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
PROJ=your-gcp-project
# 현재 외부 IP (start 할 때마다 바뀌는 ephemeral IP)
gcloud compute instances describe pg-cuvs-dev --zone=us-central1-b --project=$PROJ \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)'
# 상태 + 부착된 service account
gcloud compute instances describe pg-cuvs-dev --zone=us-central1-b --project=$PROJ \
  --format='value(status, serviceAccounts[0].email)'
# GPU 드라이버 정상 여부 (VM 안에서)
ssh ubuntu@<IP> 'nvidia-smi | head -3'
# 데몬 로그의 GPU 감지 라인
ssh ubuntu@<IP> 'sudo journalctl -u pg-cuvs-server --no-pager -n 20 | grep -i "GPU\|CUDA"'
```

---

## 3. 원인 분기 (Cause branches)

### A. Ephemeral 외부 IP 변경
`pg-cuvs-dev`/`pg-cuvs-dev-mgpu`는 정적 IP가 아니다. stop/start 시 외부 IP가
바뀐다(예: 이번 세션 `35.224.130.40` → `104.197.150.30`). `.env.gpu`의 `GCP_VM`은
이전 IP를 가리키므로 `make sync`/`gpu-*`가 죽은 호스트로 붙는다.

### B. NVIDIA driver/library version mismatch (stop/start의 주 함정)
stop/start 후 userspace NVML 라이브러리와 로드된 커널 모듈 버전이 어긋나
`nvidia-smi`가 NVML 초기화에 실패하고, 데몬은 `cuvs_detect_gpus`에서 0개를 보고
`no CUDA GPUs detected`로 종료한다. **reboot(reset)으로 일치하는 커널 모듈을 다시
로드**하면 해소된다.

### C. `/tmp` 휘발
`/tmp/cuvs_indexes`는 stop/start/reset 시 비워진다. 로컬 artifact가 사라지므로
인덱스는 (GCS snapshot이 있으면) warmup으로 다시 받거나 REINDEX가 필요하다.

### D. Service account 부재 → GCS 인증 불가
`pg-cuvs-dev`는 기본적으로 SA가 부착돼 있지 않았다. 데몬의 GCS 클라이언트는 instance
metadata에서 토큰을 못 받고 `cuvs.gcs_key_file`도 비어 있으면 GCS 업로드/다운로드가
전부 실패한다(→ `gcs-snapshot-ops.md`).

### E. 멀티 GPU VM은 비용 발생 + 별도 setup
`pg-cuvs-dev-mgpu`(a2-highgpu-2g, ~$7.35/hr)는 평소 TERMINATED. start 후 sync/build/
install/postinstall + systemd unit 재구성이 필요하고, **사용 후 반드시 stop**.

---

## 4. 복구 절차 (Recovery steps)

### IP 변경 후
```bash
PROJ=your-gcp-project
NEWIP=$(gcloud compute instances describe pg-cuvs-dev --zone=us-central1-b --project=$PROJ \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)')
echo "new IP = $NEWIP"
# .env.gpu 의 GCP_VM=ubuntu@<old> 를 ubuntu@$NEWIP 로 수정.
# 일회성으로는 make 에 override: make sync GCP_VM=ubuntu@$NEWIP
```

### Driver/library mismatch 후 (reset)
```bash
gcloud compute instances reset pg-cuvs-dev --zone=us-central1-b --project=your-gcp-project
# 부팅 후 (~45s) SSH 재개되면 확인:
ssh ubuntu@<IP> 'nvidia-smi | head -3'   # NVIDIA-SMI 와 KMD 버전이 일치해야 함
```
> stop/start 대신 가능하면 **reset/reboot**을 쓴다. 이미 mismatch가 났으면 reset이
> 정답. mismatch가 반복되면 드라이버 패키지(예: `nvidia-dkms`) 재설치 필요.

### 멀티 GPU VM 시작/setup/종료
```bash
PROJ=your-gcp-project
gcloud compute instances start pg-cuvs-dev-mgpu --zone=us-central1-f --project=$PROJ
MGIP=$(gcloud compute instances describe pg-cuvs-dev-mgpu --zone=us-central1-f --project=$PROJ \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)')
make sync gpu-build gpu-server gpu-install gpu-postinstall GCP_VM=ubuntu@$MGIP
# (systemd unit / shared_preload / extension refresh 는 gpu-snapshot/sharding 플레이북 참조)
# ... 작업 끝나면 반드시:
gcloud compute instances stop pg-cuvs-dev-mgpu --zone=us-central1-f --project=$PROJ
gcloud compute instances describe pg-cuvs-dev-mgpu --zone=us-central1-f --project=$PROJ --format='get(status)'
# 출력: TERMINATED  (+ 사용 시간 × $7.35/hr 로 비용 보고)
```

### GCS용 SA 부착 (VM stop 필요 → 이후 reset 권장)
```bash
PROJ=your-gcp-project; Z=us-central1-b
gcloud compute instances stop pg-cuvs-dev --zone=$Z --project=$PROJ
gcloud compute instances set-service-account pg-cuvs-dev --zone=$Z --project=$PROJ \
  --service-account=gpu-exp-may@your-gcp-project.iam.gserviceaccount.com \
  --scopes=https://www.googleapis.com/auth/cloud-platform
gcloud compute instances start pg-cuvs-dev --zone=$Z --project=$PROJ
# start 후 driver mismatch가 나면 위의 reset 절차 수행.
```

---

## 5. 검증 명령 (Verification commands)

```bash
# GPU 정상 (드라이버/라이브러리 버전 일치)
ssh ubuntu@<IP> 'nvidia-smi --query-gpu=index,name --format=csv,noheader'
# 예상: 0, NVIDIA A100-SXM4-40GB   (mgpu면 0/1 두 줄)

# 데몬이 GPU를 잡고 listening
ssh ubuntu@<IP> 'sudo journalctl -u pg-cuvs-server --no-pager -n 30 | grep -iE "GPU [0-9]|listening"'
# 예상: GPU 0 (... A100 ...): 40465 MB total / listening on /tmp/.s.pg_cuvs

# (SA 부착 시) instance metadata 토큰 발급되는지
ssh ubuntu@<IP> 'curl -s -H "Metadata-Flavor: Google" \
  http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/email'
# 예상: gpu-exp-may@your-gcp-project.iam.gserviceaccount.com
```

---

## 6. Escalation 기준 (When to escalate)

- `reset` 후에도 `nvidia-smi`가 mismatch면: 드라이버 패키지 버전 불일치(부분
  업그레이드). `apt list --installed | grep nvidia` 확인 후 드라이버/DKMS 재설치.
- mgpu VM이 start 후 GPU 0개만 보이면: zone capacity/quota 문제일 수 있음 — 다른
  zone 또는 시간 재시도.
- `.env.gpu` IP를 고정하고 싶으면: 정적 외부 IP를 예약해 VM에 연결(운영 정책 결정 필요).
- 비용: mgpu VM을 stop 안 하고 방치한 흔적이 보이면 즉시 stop + 사용 시간 보고.

관련: `gpu-vm-build-and-test.md`(빌드/테스트), `gcs-snapshot-ops.md`(SA/GCS),
`daemon-restart-recovery.md`(데몬 reload).

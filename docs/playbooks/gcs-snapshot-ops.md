# Playbook: GCS 스냅샷 운영 (SA/IAM · 업로드 · sharded 복원)

Phase 3C/3G.2 객체 스토리지 스냅샷의 설정/검증/복원. CAGRA 빌드 후 artifact를 GCS에
올려두고, 새 노드(heap 호환)가 풀 재빌드 없이 warmup으로 받아서 복원한다. sharded
인덱스는 `.tids`+`.shards`+N개 `.sNNN.cagra`를 한 set으로 다룬다.

---

## 1. 증상 (Symptoms)

- snapshot이 "동작 안 함": 빌드 후 GCS 버킷에 객체가 안 생긴다.
- 데몬 로그 `upload FAILED ... HTTP 404` 또는 `corrupt manifest` 또는
  `GCS download failed`.
- 새 노드에서 warmup이 안 되고 쿼리가 계속 CPU fallback.
- `HEAP COMPAT MISMATCH ... REINDEX is required`.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
PROJ=gpu-experiment-wdl-2026
# VM에 SA가 붙어 있고 토큰을 받는지 (GCS 인증의 전제)
ssh ubuntu@<IP> 'curl -s -H "Metadata-Flavor: Google" \
  http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/email'
# 데몬이 snapshot uri를 갖고 떴는지
sudo systemctl cat pg-cuvs-server | grep -i snapshot   # 또는 수동 데몬의 --snapshot-uri
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "snapshot|objstore|upload|download|manifest"
# 버킷 내용 (로컬 gcloud로)
gcloud storage ls --recursive "gs://<bucket>/**" --project=$PROJ | head
```

---

## 3. 원인 분기 (Cause branches)

### A. VM에 service account 없음 (GCS 인증 불가) — 가장 흔한 함정
데몬 GCS 클라이언트는 (1) instance metadata 토큰, 실패 시 (2) `cuvs.gcs_key_file`의
SA JSON으로 인증한다. VM에 SA가 없고 key_file도 비면 **토큰을 못 받아 업로드/다운로드
전부 실패**. (이 저장소의 `pg-cuvs-dev`는 원래 SA가 없어 3C/3D snapshot이 한 번도
실제로 동작한 적이 없었다.) → `gpu-vm-lifecycle.md`에서 SA 부착.

### B. 버킷 권한 부족
부착된 SA가 버킷에 `roles/storage.objectAdmin`이 없으면 업로드 PUT/POST/다운로드가
403/404. 버킷 생성 + SA grant 필요.

### C. snapshot_uri 미설정
업로드/다운로드는 **데몬 측 `g_snapshot_uri`**(데몬 `--snapshot-uri`)로 게이트된다.
systemd unit에 `--snapshot-uri`가 없으면 빌드해도 업로드가 안 일어난다. (백엔드
`cuvs.snapshot_uri` GUC만으로는 부족.)

### D. (구버전) HTTP 404 on upload / corrupt manifest
이전 빌드의 잠재 버그였음: 파일 업로드가 PUT을 써서 404(미디어 업로드 엔드포인트는
POST 필요), 매니페스트 파서가 pretty-print 공백을 못 견뎌 "corrupt manifest".
**`6e85b58`(PUT→POST), `4f68918`(공백 허용 JSON 파서) 이후 빌드면 해소.** 위 증상이
보이면 데몬이 옛 바이너리인지 확인하고 재빌드/설치.

### E. heap relfilenode mismatch
manifest의 `relfilenode`가 로컬 `.relfilenode` sidecar와 다르면 hard reject(다른
heap에서 만든 artifact를 로드하지 않음). 정답은 그 노드에서 REINDEX.

---

## 4. 복구 절차 (Recovery steps)

### 버킷 준비 + SA grant
```bash
PROJ=gpu-experiment-wdl-2026
SA=gpu-exp-may@gpu-experiment-wdl-2026.iam.gserviceaccount.com
BUCKET=pgcuvs-snap-$(date +%s)
gcloud storage buckets create "gs://$BUCKET" --project=$PROJ --location=us-central1
gcloud storage buckets add-iam-policy-binding "gs://$BUCKET" --project=$PROJ \
  --member="serviceAccount:$SA" --role=roles/storage.objectAdmin
# (VM에 SA가 없으면 gpu-vm-lifecycle.md 의 set-service-account 먼저)
```

### snapshot 켜고 데몬 실행
```bash
# systemd unit ExecStart에 다음 인수 추가(또는 수동 데몬):
#   --snapshot-uri gs://<bucket> --cluster-id <cluster>
# (인증은 instance metadata SA 자동; 또는 --gcs-key-file / cuvs.gcs_key_file)
sudo systemctl daemon-reload && sudo systemctl restart pg-cuvs-server
```

### sharded 인덱스 복원 (새 노드 / artifact 유실)
1. 그 노드에 `.relfilenode` sidecar가 있어야 한다(빌드 시 확장이 기록; heap 호환 식별).
2. 데몬 startup이 `.relfilenode`만 있고 base artifact가 없는 인덱스를 cold registry에
   넣고 warmup을 enqueue → `cuvs_objstore_download`가 매니페스트의 `shard_count`로
   분기해 `.tids`+`.shards`+모든 `.sNNN.cagra`를 받고 `.tids`/`.shards` SHA를 검증한 뒤
   atomically 기록 → `load_index_sharded`가 shard별 CRC로 최종 검증(fail-closed).
3. warmup 완료 전 쿼리는 NOT_FOUND→CPU fallback; 완료 후 GPU path.

---

## 5. 검증 명령 (Verification commands)

```bash
# 업로드 성공: 데몬 로그 + 버킷 객체
sudo journalctl -u pg-cuvs-server --no-pager | grep "sharded snapshot complete"
# 예상: [INFO] [objstore] sharded snapshot complete <db>/<idx> shards=2 ts=... relfilenode=...
gcloud storage ls "gs://<bucket>/**" --project=gpu-experiment-wdl-2026 | grep -E "index\.(tids|shards|s0)|manifest"
# 예상: index.tids / index.shards / index.s000.cagra / index.s001.cagra / manifest.json (versioned + latest)
```
```bash
# 다운로드/복원 성공 (새 노드 또는 artifact 삭제 후 데몬 재기동)
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "sharded download verified|loaded sharded index"
# 예상: [INFO] [objstore] sharded download verified OK for <db>/<idx> (2 shards)
#       [INFO] pg_cuvs_server: loaded sharded index <db>/<idx> (... vecs, 2 shards)
```
```sql
-- 복원 후 상주 + top-k == CPU exact
SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='<idx>' AND resident;  -- 예상: shard 수
```
> 검증된 round-trip(이번 세션): upload → wipe local(.relfilenode만 남김) → warmup
> download → `resident_shards=2` + top-k가 CPU exact와 일치.

---

## 6. Escalation 기준 (When to escalate)

- 토큰은 받는데 업로드가 404/403: 버킷 존재 + SA `objectAdmin` 재확인. 그래도면
  버킷 리전/uniform bucket-level access 정책 점검.
- `corrupt manifest`/`HTTP 404 upload`가 최신 빌드에서도 나면: 데몬 바이너리가
  fix 커밋(`6e85b58`/`4f68918`) 이전인지 `gpu-server`로 재설치 후 재확인.
- heap relfilenode mismatch 반복: 그 노드의 heap이 정말 호환인지(같은 dump/replica인지)
  확인. 호환이면 REINDEX, 아니면 그 노드에서 새로 빌드.
- `.delta`/`.tombstone`/`.stale`은 스냅샷에서 제외된다(파생/휘발). 복원 후 write 정합은
  `drop-and-write-path-diagnosis.md` 참조.

관련: `gpu-vm-lifecycle.md`(SA/IP), `multi-gpu-sharding-ops.md`, `persistence-corruption-recovery.md`.
설계 근거: ADR-013(object storage), ADR-024(sharded snapshot).

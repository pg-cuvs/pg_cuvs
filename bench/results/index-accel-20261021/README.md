# index-accel L40S 검증 기록

GPU 측정과 원 자료 회수를 마쳤고 전용 VM은 STOPPED다. 원 그래프의 최종 SHA와 원격574개 파일의 로컬 SHA를 검증했다. 코드와 수치는 이 L40S 조건의 내부 검증이며 공개 그리드 헤드라인으로 사용하지 않는다.

- [GUC 기본값과 #144](guc-and-issue144.md): 기본값0 smoke와3회 recall·Jaccard·실제 커널 trace.
- [RMM 크기·피크·빌드](rmm-study.md): 6크기×N=3,3종 peak,CREATE INDEX0/512×N=3,물리/논리 VRAM,할당 호출 수와기본값 제안.
- [적용 범위·성장·VRAM 회계](rmm-scope.md), [제안 JSON](rmm-default-proposal.json), [VM 중지 관측](vm-stop-observation.json).

## 주요 결과

- GUC0의 두 검색점 recall/p50은 고정 baseline3회 관측 span 안에 들었다. RMM0도3회 평균 기준으로 통과했다. 원 individual gate 실패를 삭제하지 않았다.
- #144의 batch recall 저하는 itopk128부터3회 모두 나타났다. 별도 trace는 single=multi_cta_search,batch1000=single_cta_search로, 기존 이슈의 모드 명칭과 구분했다.
- 검색 p50은 initial512 MiB부터 평탄했다. 제안은 initial512 MiB + total CUDA VRAM50% upstream quota이며 private native3경우에서 검산했다. 현재 production source 기본값0/max미설정은 변경하지 않았다.
- CREATE INDEX N=3 평균±SD: pool0=46.442748±0.471304초,pool512=45.199552±0.820798초. 공개 성능 배수로 일반화하지 않는다.

## 재현 조건

baseline `8b73d65`,GUC `0caa648`,RMM `fded8ce`. CUDA bootstrap은 cuvs-bench PR#2872 문서 head77c6601 절차에서 pg_cuvs source를 baseline으로 고정하고 GPU 아키텍처를sm89로 바꿨다. wiki_all_1M×768,제공GT,10,000 query다. gd32/igd128/ivf_pq 원 graph를 smoke/#144/검색크기비교에서 재사용했고, 추가 빌드는 별도 native 프로세스와 복제 DB를 사용했다.

순서·N·후보 선정 기준은 rmm-study-protocol.json에 측정 전에 고정했다. 하네스/수정 patch는 harness/와 root patch 파일에 있으며, source snapshot과 데이터를 확인한다. 실패별 원 로그와 수집기 수정 내역은 diagnostics/,logs/에 있다. export-verification.json과 remote-evidence-map.json은 원격 파일의 크기·SHA와 저장 위치를 연결한다.

## VM

| 항목 | 관측 |
|---|---|
| 이름 / ID | pg-cuvs-index-accel-20261020 / v6nc08sjs |
| 접속 | SSH alias pg-cuvs-index-accel-20261020,실행 중 IP160.211.47.183,user ubuntu |
| GPU | Crusoe l40s-48gb.1x,L40S48GB,sm89,NVML46068MiB |
| CPU / RAM | 8vCPU / Linux144.399GiB |
| 디스크 | virtio vda128GiB(root ext4),vdb120GiB(/data). 게스트NVMe 미검출,실제backing매체 미확인 |
| 소프트웨어 | driver565.57.01,nvcc12.4.131,cuVS/RMM26.04.00,PG16.15,pgvector0.8.7 |
| 단가 | 표시 컴퓨트 단가$1.74/h,stoppable |
| 상태 | STOPPED,2026-10-20T16:56:25.455172+00:00 관측 |
| 가동 관측 구간 | 약2.0793시간,표시 단가 기준 약$3.62 |

가동 시간은 생성 성공 첫 관측부터 STOPPED 관측까지의 상한 추정이고 실제 청구서가 아니다. 스토리지 가격은 확인하지 않았다. 원 graph3개 파일은 VM의 /home/ubuntu/index-accel-validation/graph-snapshot/에 SHA 검증한 복사본으로 남겼다. /tmp 파일이 사라지는 재개 환경에서는 해당 복사본을 원 /tmp/cuvs_indexes 경로로 복원한 뒤 GPU 검증을 시작해야 한다. PostgreSQL과 daemon은 정상 종료했고 GPU 프로세스0을 관측했다. 소유한 daemon 설정은 RMM0으로 되돌렸다. VM과 데이터는 삭제하지 않았다.

기존 pg-cuvs-crusoe-test 및 보존 index15335762는 사용하지 않았다. 예정 A100(sm80) 그리드의 검증은 하지 않았다. PR 생성은 .claude/pr-policy.json 미등록 자동 훅으로 막혀 있으며 해당 정책 등록의 직접 사용자 승인 응답이 없다. raw 결과 커밋의 공개 push는 보류했다.

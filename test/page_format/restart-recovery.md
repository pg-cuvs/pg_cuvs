# CAGRA 판 저장과 재시작 회귀

## 후속 export와 brute-force 연결

- PG 메타와 일치시킬 키는 `(sysid, db_oid, index_oid, index_nonce, base_nonce, graph_nonce, L, F)`다. 데몬에서 `L`은 `PageCagraGraph.n_slots`, `F`는 `folded`다.
- `CuvsPageGraphBoundary.graph_rows`는 실제 그래프의 행 수 `L_g`다. 온전한 판은 `L_g = L`과 `proxy_rows == NULL`을 만족한다. 대역 판의 그래프는 `[0, L_g)`이고, `proxy_rows`는 `[L_g, L)`에 해당하는 slot 순서의 float32 벡터다. 대기 행은 별도 위치 공간 `[F, T)`에 있다.
- 후속 export 작업은 출처 판의 식별자와 PG 생애를 확인한 뒤 `page_graph_fold_pin_locked`로 핸들을 잡는다. 레지스트리 잠금을 푼 뒤 물리 행 수를 재확인하고 `[0, L_g)`만 내보낸다. FD 응답과 오류 정리가 끝나면 `page_graph_fold_unpin`으로 놓는다. 응답에는 `L_g`와 그래프 데이터만 싣는다. TID는 PG의 FCL 캡처가 주며, 후속 export 작업이 `[L_g, L)`와 `[F, T)`를 PG에서 채운다.
- brute-force 공급은 같은 pin으로 물리 dataset의 실제 행 수와 `L_g = L`을 확인한 뒤에만 전수 자료로 물질화한다. 대역 판의 `[L_g, L)`는 별도 범위다. 자리표시로 재유도·재수화한 판은 `placeholder_boot`가 요청의 `boot_id`와 같은 경우에만 쓴다.
- 이 변경은 경계 타입과 판 수명을 제공한다. export opcode, FD 응답의 경계 추가, PG 캡처를 통한 그래프 밖 행 삽입, 네 모드와 타임아웃은 후속 export 작업에서 연결하고 검증한다. brute-force 공급과 이 브랜치의 결합도 별도 실행이 필요하다.

## 복구 등록과 저장 권위

- PG 워커는 카탈로그에서 유효한 WAL 인덱스를 열고 접기 기록을 읽어 현재 head와 조상을 확인합니다. 시작 요청은 시스템 식별자, boot, 두 판의 신원과 경계, relfilenode, 플러시한 WAL 위치를 묶습니다.
- 데몬은 GPU 작업을 마친 뒤 새 핸들을 비공개로 유지합니다. 워커는 같은 소켓의 데몬 생애와 일회성 응답을 대조하고, 짧은 FCL 구간에서 boot, relfilenode, 메타 head를 다시 확인한 뒤 승인합니다. 데몬은 승인 기한과 해당 인덱스의 권위 갱신·DROP 세대를 등록 직전에 확인합니다.
- 신뢰 경계는 전용 서비스 계정, 0660 Unix 소켓과 `SO_PEERCRED`로 확인되는 OS 피어·전달 FD 소유권입니다. 이 경계 안에서 PG 워커의 동일 연결 승인으로 등록과 퇴역을 시점에 결속합니다. 별도의 PG 역할 인증과 데몬의 독립 WAL 검증은 지원하지 않습니다.
- 복구 작업은 화신 nonce로 파일 경로를 제한하고 같은 DB·인덱스의 저장 게이트를 사용합니다. 페이지 DROP은 그 인덱스의 게이트만 잡습니다. legacy DROP은 해당 OID의 진행 중 등록을 무효화하고 저장 게이트를 기다리지 않습니다. 전역 레지스트리 잠금은 pin, 메타 비교, 등록 갱신에 사용합니다. 파일 직렬화·CRC·fsync와 GPU 준비 중에는 잠금을 놓습니다.
- 체크포인트 교체 뒤 부모 fsync가 실패하면 이전 디렉터리를 원래 이름으로 교환합니다. 롤백 교환도 실패하면 이전 디렉터리를 임시 이름 아래 보존합니다. 요청한 루트가 돌아온 뒤 저장을 다시 시도합니다. 상주 판에 연결된 정확 체크포인트가 사라진 경우에는 대조가 저장 상태를 비워 재저장을 허용합니다.

복구 런처는 페이지 형식과 접기 옵트인을 함께 켠 primary에서 기존의 데이터베이스별 동적 복구 워커를 재사용합니다. 레거시 auto-compaction은 `cuvs.auto_compact_database`로 지정한 한 데이터베이스에서만 동작하고 설정이 비어 있으면 종료합니다. 복구 런처는 공유 카탈로그로 여러 데이터베이스를 찾아 부팅 뒤 WAL 인덱스의 권위 확인·복구·대조를 배정합니다.

## 자리표시 보호와 이전 boot 퇴역

- manifest v2는 CRC 범위 안에 자리표시 플래그, PG boot, 확인한 물리 무효 칸 하한을 보존합니다. 재수화와 대역 판의 다음 접기는 보호를 상속합니다. 같은 boot에서 체크포인트를 적재해도 무효 칸 과소 집계 또는 보이는 결과 부족 시 정확 힙 경로가 작동합니다. 다른 boot의 파일과 보호 증거가 없는 개발판 v1은 네이티브 적재 후보에서 제외됩니다. v1의 키와 CRC는 대조용으로만 읽습니다.
- 같은 화신의 이전 boot 그래프가 현재 WAL head를 서빙할 수 없으면 PG 워커는 복원 파일 선택과 GPU 준비 전에 별도 승인으로 등록부에서 떼어 냅니다. 현재 boot의 대기 판과 이미 사용할 수 있는 현재 판은 보존합니다. 종료 응답으로 PG의 FCL을 먼저 놓고, 인덱스 저장 게이트 안에서 미보유 핸들을 해제합니다. 보유된 page graph의 VRAM은 마지막 pin 이후의 실제 해제까지 청구합니다.
- 새 준비는 가용량을 다시 계산한 뒤 예약합니다. 승인은 레지스트리 갱신과 예약→상주 청구 전환을 같은 잠금 구간에서 처리합니다. 중단되거나 현재 boot의 대기 판이 먼저 서빙을 시작하면 준비 핸들을 해제한 뒤 예약을 반환합니다.

## 체크포인트 내구성과 파일 회수

- 파일 fsync 뒤 staging 폴더 fsync, 디렉터리 rename 뒤 상위 폴더 fsync 순서를 유지합니다. rename의 성공은 캐시 이름만 바꿉니다. 복원과 회수는 PG 메타의 WAL head 및 조상 승인에 따릅니다.
- 파일 이름은 정규 16자리 소문자 hex로 제한하고 소유한 일반 파일과 받은 inode/ctime을 재대조합니다. 외부 이동 후 같은 이름에 들어온 디렉터리는 기존 승인에서 제외합니다.
- manifest CRC·필드·잘림은 거절 오라클로 검사하고, 호출 순서는 데몬과 독립적인 Linux syscall 기록으로 확인합니다. SIGKILL과 syscall 성공만으로 전원 장애 때의 파일시스템 내구성을 판정할 수 없으므로, rename 직후 네임스페이스 소실은 사본 이동으로 따로 모델링합니다.

## 판 디렉터리 대조

PG 워커는 현재 화신의 판 디렉터리를 128개씩 nonce 순으로 끝까지 읽습니다. 목록을 받은 뒤 카탈로그의 현재 head와 플러시된 전체 접기 계보를 대조하고, 후속 페이지와 회수 요청 시점마다 동일 head를 확인합니다. 대조 중 생기거나 바뀐 파일은 다음 주기에 다시 봅니다.

- 현재 nonce와 충돌하거나 WAL에 기록된 같은 nonce의 `(L,F)`와 다르면 보존합니다. manifest와 생성 XID 표식이 모두 맞고, 동일 시스템·DB·인덱스·화신·base·벡터 모양을 증명해야 회수 후보가 됩니다.
- 전체 WAL에 없는 판은 과거 분기 후보입니다. WAL 조상은 현재 판의 boot 독립 체크포인트가 CRC를 통과하고 파일과 상위 디렉터리의 fsync가 끝난 뒤에만 대체 가능합니다.
- 생성 full XID가 현재 epoch 범위에서 종료됐음을 PG가 확인해야 합니다. 미확정·진행 중·미래 XID는 보존합니다. 데몬은 회수 직전 동일 디렉터리의 device, inode, ctime, manifest CRC와 상주·복구 참조를 재확인합니다. 소유권이나 파일 종류가 모호한 디렉터리도 보존합니다.
- `reconcile` 회귀는 두 번째 목록 페이지까지 132개 과거 분기와 대체 가능한 조상 하나의 회수 판정을 검증합니다. 테스트 데몬의 dry-run에서 판정만 기록하고 체크포인트들이 남아 있는지 확인합니다.

## Linux CPU

```sh
make test-page-recovery
make test-page-recovery-lifetime
make test-page-format PG_CONFIG="$PG16_BIN/pg_config"
```

첫 명령은 계보 산술과 그래프·칸·대기 결과 번호, manifest 보호, 파일명 allowlist를 검사합니다. 수명 셀은 실제 page graph 레지스트리와 회계 함수에 CPU 핸들을 붙여 pin 보유 중의 차지, 중단 반환 및 예약 전환, 현재 boot의 대기 판 보존을 확인합니다. PG16 CPU-shim 페이지 매트릭스와 `off` 셀은 기본 빌드와 `CUVS_PAGE_FORMAT=1 PGCUVS_BETA3A_GPU_FOLD=0` 조합을 구분합니다.

## Linux A100

승인된 전용 시험 환경에 PG16, pgvector, cuVS 접기 SDK를 준비한다. 설치하는 확장과 데몬은 같은 소스에서 `CUVS_PAGE_FORMAT=1 PGCUVS_BETA3A_GPU_FOLD=1`로 빌드한다. FIFO·dry-run에는 `pg_cuvs_server_test`를 사용한다. 취소 VACUUM 셀은 `CUVS_TEST_HOOKS`를 켠 확장도 요구한다. `save-crash-*`는 자체 데몬을 ptrace할 수 있는 Linux `strace`(`-D -ff -yy` 지원)가 필요하다. 추적이 막히면 데몬 로그만으로 합격시키지 않는다. 각 명령에 고유한 로그 디렉터리를 준다. 셀마다 사설 PG 클러스터를 만든다.

```sh
for cell in shutdown-install shutdown-install-timeout \
    normal proxy corrupt manifest foreign absent reconcile cancel-vacuum \
    delete-visibility root-return saved-placeholders retire-pressure reconcile-move \
    manifest-fields manifest-truncated manifest-v1 save-crash-before-staging \
    save-crash-after-staging save-crash-after-rename save-crash-rename-lost save-crash-after-parent; do
    python3 -B test/page_format/restart_recovery.py "$PG16_BIN" "$DAEMON_TEST" \
        --case "$cell" --logs "$LOG_ROOT/$cell" || exit
done
python3 -B test/page_format/restart_recovery.py "$PG16_OFF_BIN" "$DAEMON_OFF" \
    --case off --logs "$LOG_ROOT/off"
```

| 셀 | 판정 |
| --- | --- |
| `shutdown-install`, `shutdown-install-timeout` | 복구 GPU 핸들을 게시 직전 FIFO에 붙든다. SIGTERM 뒤 FIFO를 풀면 정상 종료하고, 풀지 않으면 제한 시간 뒤 정상 CUDA 종료 경로를 거치지 않고 종료 코드 1로 끝난다. 두 경우 모두 PostgreSQL의 게시 head를 보존해야 한다. |
| `normal` | 접기 두 번 뒤 현재 판을 저장한다. SIGTERM과 PG 정상 재시작 뒤 정확한 체크포인트 적재를 확인한다. |
| `proxy` | 두 번 접은 뒤 SIGKILL한다. 최초 판만 돌려놓고, 게시 직전 FIFO에서 검색이 정확 경로인지 본다. FIFO 중 무관 legacy DROP의 응답을 확인하고, 재유도 뒤 슬롯 512~767의 검색과 그 위의 접기를 대조한다. |
| `corrupt`, `manifest` | 아티팩트와 manifest를 각각 크기 유지 상태로 변이한다. 재수화로 돌아오고 결과가 유지되는지 확인한다. |
| `foreign` | 다른 nonce와 그에 맞는 CRC를 가진 조상 후보를 둔다. PG 접기 기록에 없어 재수화가 선택돼야 한다. |
| `absent` | 아티팩트를 제거한다. PG 권위만으로 재수화하고, 데몬을 둔 채 PG를 다시 시작하여 돌아오는지 확인한다. |
| `reconcile` | 다중 목록 페이지, 진행 중·미래 XID, 다른 시스템, 불일치 표식과 WAL nonce, 알 수 없는 파일의 보존을 dry-run으로 검사한다. |
| `cancel-vacuum` | F=0에서 취소된 VACUUM이 슬롯만 무효화한 사실을 raw page로 확인한다. 재수화 후 정확 힙으로 전환하며, PG가 죽은 칸 수를 게시하면 GPU 검색이 재개돼야 한다. |
| `delete-visibility` | VACUUM 없이 DELETE한다. 기존 PG 스냅숏은 삭제 행을 GPU 경로에서 볼 수 있어야 하고, 그 스냅숏이 종료된 뒤에도 재수화와 GPU 검색이 완료돼야 한다. |
| `root-return` | 체크포인트 원본 루트를 보존한 채 빈 루트를 같은 위치에 둔다. 상주 판이 그 루트에 다시 저장돼야 한다. |
| `saved-placeholders` | 같은 boot에서 접기 판을 적재한 뒤 `k=8`의 GPU 결과 8개를 먼저 확인한다. 같은 세션에서 한 행만 보이도록 DELETE하고 PG 메타가 그대로인 상태에서 동일 질의를 다시 실행한다. 힙 오라클과 같은 행을 반환하고 `pg_cuvs_last_search_k()`와 `pg_cuvs_last_search_n_results()`가 모두 NULL이어야 한다. DELETE는 롤백한다. 새 boot는 힙에서 다시 증명해야 한다. |
| `retire-pressure` | 66 MiB·레지스트리 1칸에서 daemon을 유지한 채 PG를 재시작합니다. 예전 그래프의 pin을 붙든 동안 새 준비가 거절되고 pin을 놓으면 같은 예산에서 GPU가 재개돼야 합니다. |
| `reconcile-move` | LIST 이후 디렉터리를 외부로 이동하고 같은 이름에 새 inode를 둡니다. 이전 승인과 비정규 이름은 회수 대상에서 빠져야 합니다. |
| `manifest-fields`, `manifest-truncated`, `manifest-v1` | 다시 계산한 CRC가 있는 모순된 보호 필드, 잘린 길이, 보호 정보가 없는 이전 판의 네이티브 적재를 각각 거절합니다. |
| `save-crash-*` | FIFO로 중단 지점을 선택한다. `checkpoint-syscalls.<tid>`의 성공한 호출에서 해당 staging 디렉터리를 만든 스레드 한 개를 찾아 아티팩트·manifest·XID 표식 fsync, staging fsync, 게시 rename, 상위 세 디렉터리 fsync의 순서와 중단점까지의 범위를 확인한다. 재시작 뒤 WAL head와 힙 오라클을 대조한다. |
| `off` | 판 회복 작업자와 manifest가 생기지 않고 기존 검색이 고정된 최근접 ID 337을 돌려주는지 본다. |

각 검색의 결과 행은 예상 정수 ID와 힙 순차 스캔 결과에 모두 맞아야 한다. 부분 인덱스에는 질의 벡터와 정확히 일치하지만 술어가 거짓인 행을 넣고, 그래프 행 수와 최근접 결과에서 모두 빠지는지 확인한다. 인덱스 경로에서 `pg_cuvs_last_search_k()`를 별도로 확인한다. 재시작 뒤 PG 메타의 nonce와 경계가 그대로인지 raw page로 검산한다. 파일 검사는 Python의 `zlib.crc32`로 manifest와 아티팩트를 직접 읽는다. 표현식 함수는 테이블 소유자만 평가할 수 있게 만들어 복구 작업자의 실행 권한 변경도 확인한다. A100 1M×768의 G7·G10 시간과 운영 판정은 이 소형 기능 회귀와 별도로 남긴다.

## 코드 변이

각 변이는 같은 Linux 소스의 별도 복사본에서 한 번에 하나만 적용하고 재빌드한다. 원본과 변이본 각각의 명령, 종료 코드, 첫 실패 메시지를 남긴다.

| 변이 | 실행과 기대 실패 |
| --- | --- |
| `graceful_shutdown`에서 `page_recovery_drain_shutdown_locked` 호출을 뺀다. | `shutdown-install`에서 FIFO를 놓기 전에 종료한다. |
| `finish_page_graph_dispose` 전에 퇴역 소유권을 반환한다. | `make test-page-recovery-lifetime`에서 후보 또는 마지막 pin의 GPU 해제 시점에 소유권이 없어서 실패한다. |
| `cuvs_page_recovery_lineage`에서 `L-L_v = F-F_v` 검사를 뺀다. | `make test-page-recovery`의 `wrong_folded`가 실패한다. |
| `cuvs_page_proxy_score`의 결과 번호를 `slot - graph_rows`로 바꾼다. | `make test-page-recovery`에서 칸 2와 3의 독립 단언이 실패한다. |
| `validate_page_checkpoint`에서 아티팩트 CRC 비교를 뺀다. | `corrupt`가 재수화 경로 판정에서 실패해야 한다. cuVS가 변이 위치의 파일을 자체 거부해 같은 경로를 선택하면 이 변이는 구별되지 않은 것으로 기록한다. |
| `read_page_manifest`에서 manifest CRC 비교를 뺀다. | `manifest`가 체크포인트 거절 및 재수화 판정에서 실패한다. |
| `handle_page_overlay_search`에서 `cuvs_page_proxy_score` 호출을 뺀다. | `proxy`의 재시작 뒤 첫 힙 대조가 실패한다. |
| `page_fold_run`에서 `proxy_count + event.positions`의 복제 입력을 `event.positions`로 바꾼다. | `proxy`에서 접기 이후 물리 행 번호가 어긋나 검색 대조가 실패한다. |
| `cuvs_cagra_spool_recovery`의 테이블 소유자 전환을 뺀다. | `absent`에서 힙은 계속 답하고, GPU 복귀 대기가 시간 초과한다. |
| 회복 런처 등록을 뺀다. | `normal`에서 현재 체크포인트와 정확한 힙 결과가 있어도 GPU 복귀 대기가 시간 초과한다. |
| `cuvs_page_checkpoint_can_prune`에서 WAL 조상에 필요한 정확 체크포인트 확인을 뺀다. | `make test-page-recovery`에서 replacement 없는 조상 보존 단언이 실패한다. |
| 생성 full XID의 범위나 PG 진행 상태 확인을 뺀다. | `reconcile`의 미래 또는 진행 중 XID 보존 단언이 실패한다. |
| `page_overlay_search_status`에서 명시적으로 확인한 물리 무효 칸의 하한 비교를 뺀다. | `cancel-vacuum`에서 저평가된 메타 카운트로 GPU 경로가 선택돼 실패한다. |
| `recovery_vector`의 전역 사망 분기를 없앤다. | `delete-visibility`에서 오래된 스냅숏이 끝난 뒤의 재수화가 멈춘다. |
| legacy DROP도 전역 페이지 저장 게이트를 기다리도록 되돌린다. | `proxy`의 FIFO 구간에서 unrelated legacy DROP이 응답 기한을 넘긴다. |
| 체크포인트 적재 시 `PageCagraGraph.recovery_placeholders`에 manifest의 보호 비트를 이어 주는 항만 뺀다. boot 제한과 등록 로그는 둔다. | `saved-placeholders`에서 살아 있는 행 500이 우연히 반환돼도 `route-after`가 GPU 경로라서 실패한다. 다른 boot의 파일 수용은 별도 단언이 검사한다. |
| 권위 승인 후에도 VRAM 수용 뒤에 이전 boot를 퇴역시킨다. | `retire-pressure`에서 초기 퇴역 사건이 사라지고, pin이 풀려도 기존 등록판 때문에 재수용이 막힙니다. |
| 퇴역 시 실제 해제 전에 page graph 청구를 반환한다. | `test-page-recovery-lifetime`과 `retire-pressure`의 보유 중 수용 거절이 실패합니다. |
| 목록 이후의 물리 디렉터리 신원 재확인을 뺀다. | `reconcile-move`에서 교체 inode가 이전 회수 승인에 들어갑니다. |
| `persist_page_checkpoint`의 staging fsync 호출 또는 rename 직후의 `sync_page_incarnation` 호출을 각각 성공 상수로 대체한다. FIFO와 단계 로그는 그대로 둔다. | `save-crash-after-parent`가 해당 성공 syscall이 없는 순서 기록으로 실패한다. 상위 디렉터리 3개 중 하나만 빠져도 같은 판정이다. |

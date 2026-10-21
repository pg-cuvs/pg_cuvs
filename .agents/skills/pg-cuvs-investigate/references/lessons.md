# 조건별 조사 판단

해당 경계가 의심될 때 필요한 항목만 읽는다. 아래 절차는 특정 실행의 결과나 현재 작업의 지시를 대신하지 않는다. 관측·추론·미확정을 구분하고 실제 사용한 버전과 입력에 적용 범위를 제한한다.

## 보존·도달성과 유한 탐색

vector·label·간선이 보존되고 entry에서 모든 node에 도달해도 유한 탐색 폭에서 miss가 날 수 있다. payload 손상, 단절, 후보 graph와 검색 정책의 상호작용을 별도 가설로 둔다.

같은 graph에서 탐색 폭만 바꾸거나, 진단용 복사본의 특정 간선만 바꾸는 대조를 선택할 수 있다. 변화시킨 요인과 나머지 바이트·ID·검색 조건을 기록한다. 회복이 관측돼도 원 설정의 실패와 생성 구현의 책임은 남는다. 작은 sanity query의 성공을 고정 query 집합이나 전체 품질로 확대하지 않는다.

## 입력 재생성과 byte 동일성

생성기 버전·플랫폼·dtype 변환이 다르면 같은 seed와 shape에서도 전체 입력이 달라질 수 있다. 실행 전 고정 범위의 전체 내용 지문을 확인하고, sample 일치와 전체 일치를 구분한다.

불일치하면 expected를 새 입력에 맞춰 조용히 바꾸지 않는다. 고정 바이트를 전달하거나 새 입력의 별도 실험으로 선언한다. 사후 재생성은 실제 과거 query·입력 바이트를 관측해 복구한 것과 다르다.

## UNLOGGED page와 shared buffer

live relation의 파일 bytes와 엔진이 보는 page가 다를 때 적용한다. 디스크의 zero page나 오래된 tuple만으로 converter 손상을 확정하지 않는다.

해당 PostgreSQL 버전의 checkpoint·UNLOGGED 처리 계약을 읽고, engine page를 조회하거나 소유한 격리 relation을 적절히 flush·정상 종료한 뒤 비교한다. 일반 CHECKPOINT가 모든 dirty page를 같은 방식으로 반영한다고 가정하지 않는다. 기존 파일·실패와 관측 시점 차이를 보존한다.

독립 decoder로 비교했다면 어느 파일 상태를 읽었는지 명시한다. 특정 relation의 flush 후 일치를 전체 서버의 내구성·동시성 증명으로 확대하지 않는다.

## 최초 실패의 보존

비교 전에 expected ID·ordinal·query 바이트 또는 지문, 실제 반환값·거리·설정·입력·산출물 identity를 저장한다. 저장 확인에 실패하면 그 결과를 후속 비교에 사용하지 않는다. 원 예외와 기록·정리 오류를 별도로 남긴다.

사라진 진단값은 로그나 준비 검토를 늘려도 관측값으로 돌아오지 않는다. 사후 가설과 확인된 사실을 나누고, 필요한 최소 계측을 갖춘 다음 실험으로 가설을 구별한다. 기존 실패를 성공한 다른 설정으로 덮지 않는다.

## PG 재시작 뒤 과거 integrity 증거의 역할

같은 PGDATA·relation OID나 현재 PID만으로 과거 receipt의 유효성을 판단하지 않는다. 실제 collector와 workload verifier가 요구하는 epoch·데이터 보존·postmaster identity 계약을 함께 읽는다.

과거 receipt는 당시 성공 이력으로 보존하고 현재 증거와 별도로 결속한다. 과거 baseline의 identity를 현재값으로 덮어쓰지 않는다. 재검증은 UNLOGGED 여부·재시작 경과·현재 보존 증거에 맞게 정하며, 모든 재시작에 전체 scan을 자동 추가하지 않는다.

## 지원 도구의 범위

helper·parser·fixture·배포 도구의 실패와 제품 실행의 실패를 구분한다. 필요한 소비자 계약을 읽고 실제 결함이 있는 경계를 좁게 고친다. 관측 도구를 수정한 준비 PASS는 전체 기능 PASS가 아니다.

작업이 끝나는 조건은 새로 구별한 가설과 기능 결과다. 원인 조사보다 큰 source closure나 운영 프레임워크를 새로 만드는 것으로 진척을 대신하지 않는다. 불명 상태를 무시해 통과시키는 것도 수정이 아니다.

## 진단 frame과 ACK frame의 서로 다른 한도

여러 decoder가 같은 stream을 읽거나 진단 레코드를 추가할 때 적용한다. 단독으로 정상인 진단 행도 ACK framer·tail pending buffer·chunk 한도를 넘을 수 있다. 각 소비자의 실제 byte/event 한도와 frame 인코딩을 함께 확인한다.

저장된 혼합 원문을 실제 연결 소비자에 넣어 검증한다. 인식한 진단 frame을 보조 채널로 전달하더라도 원 decoder의 최종 검증과 control ACK의 제약을 유지한다. unknown·malformed·partial frame이나 control marker 혼합을 허용하는 우회로 만들지 않는다.

oversized 행이 newline에서 끝났으면 다음 정상 행까지 discard하지 않는지 확인한다. frame 경계에서 나뉜 입력과 여러 frame이 한 chunk에 들어온 입력을 구분한다. 원문 CPU replay 성공은 당시 commit·query·fresh 실행을 소급 증명하지 않는다.

## RPC timeout 뒤 늦은 응답과 최초 오류

일부 frame이나 지연 응답이 남은 stream을 다음 요청에 재사용하면 이전 응답을 새 응답으로 오인할 수 있다. 실패한 stream은 후속 정상 요청에 재사용하지 않고, close 오류가 최초 timeout을 덮지 않게 한다.

응답 준비·queue 등록·실제 전송을 구분한다. 선택적 동기 작업이 전송보다 앞에 들어갔는지 실제 event loop와 caller를 확인한다. 작은 유한 재현에서 정상 지연·잘못된 ID·연속 요청·실패 통지·정리까지 대조한다. timeout 증가나 단일 함수 성공만으로 전체 경로를 검증했다고 하지 않는다.

후속 오류에는 operation·kind·기대/관측 identity·읽기 단계·수신 길이·한도와 bounded 원문 지문을 남긴다. 과거 wire가 없으면 역사적 첫 원인은 미확정으로 둔다. fatal 통지 뒤 최종 receipt가 누락되면 normal payload 생성과 별도 기록 경로가 서로 막는지 확인하며 누락된 ACK나 receipt를 합성하지 않는다.

전송 전에 실제 argv 크기와 렌더링된 문법을 확인한다. 소비하지 않는 대형 로그 객체를 argv에 넣지 않는다. template 치환이 이미 삽입한 데이터까지 바꾸지 않도록 구조화한다. 원격 root 부재나 parent 종료는 그 시점의 관측이며 지연 실행 부재·재시도 권한의 증명이 아니다.

## 설정 소비와 검색 재생

GUC readback이 바뀌어도 결과가 같을 때는 실제 AM callback·바이너리 등록 포인터·소비 변수를 연결한다. 작은 행동 대조를 선택하되 filter·한 node의 heap TID 개수·iterative scan 여부가 달라지면 반환 행 수를 탐색 폭으로 해석하지 않는다. 정적 연결, 행동 변화, 실제 인자 캡처는 서로 다른 증거다.

재생은 native reader의 sentinel·version·count와 이웃 slot 순서, visited 처리와 heap 동점을 맞춘다. 첫 invalid slot 뒤의 값으로 경로를 만들지 않는다. 동점 comparator가 거리를 같다고 반환하면 임의의 block·ID tie-break를 넣지 않는다. 원 source의 strict 비교와 후보 유지·확장·종료 조건을 그대로 확인한다.

거리 oracle은 가능한 한 원 실행의 실제 support 함수를 사용한다. 내부 squared L2와 SQL sqrt-L2, 입력 정밀도와 누산·반환 정밀도를 구분한다. SQL 거리의 제곱이나 다른 reduction으로 원 squared 값을 복원했다고 가정하지 않는다. query literal·wire·index payload의 바이트 대응을 검증한다.

재생 결과는 원 native의 관측 가능한 반환·후보·prefix와 비교한다. endpoint 일치만으로 내부 trace 전체가 같다고 하지 않는다. source model에서 재구성한 상태를 캡처한 native 내부 trace로 표시하지 않는다. 표본 page 일치를 전체 graph의 보존 증거로 쓰지 않는다.

반환 행 수·방문 수·거리 계산 수·확장 수를 구분한다. 비용 카운터의 증가 위치를 읽고, 이웃 수 합계나 추가 SQL 검산을 native 거리 호출 수와 혼동하지 않는다. 실제 계측을 추가하면 원 함수·인자와 계측 전후 반환 ID·거리 bits의 보존을 확인한다. 순차 시간에는 cache·계측 비용이 섞일 수 있다.

최종 bound는 시간에 따라 변한다. 더 먼 node가 일찍 수용·확장됐을 수 있고, retained 집합에서 탈락해도 별도 확장 큐에는 남을 수 있다. 한 경로의 미확장 node와 모든 대체 경로의 장벽을 구분한다. 종료점의 탈출 장벽을 entry부터 모든 경로에 필요한 장벽으로 해석하지 않는다.

entry를 바꾸는 대조는 graph·query·거리·검색 폭을 고정한다. 더 가까운 entry가 유한 탐색의 최종 결과도 개선한다고 가정하지 않는다. target을 알고 고른 entry의 회복은 지역 효과의 증거이며 target을 모르는 실제 선택 정책이나 전체 품질의 증명이 아니다.

## 고정 graph의 독립 검색 대조

같은 fork를 감싼 여러 caller는 독립 구현이 아니다. 기준 구현의 정확한 revision과 header bytes를 고정하고 같은 저장 graph·query·거리·entry·검색 조건으로 비교한다. 먼저 저장 형식과 vector·label 대응이 호환되는지 확인한다.

필요한 범위에서 전체 후보 ID·거리 bits·실제 callback 횟수·평가 순서 지문을 대조한다. 지문 일치를 전체 원문 stream 대조로 부르지 않는다. 명목상 같은 탐색 폭이라도 heap·tie·수치 reduction의 의미가 같은지 별도로 확인한다.

일치하면 해당 고정 graph에서 검색 구현 고유 차이라는 가설이 약해진다. 생성·변환·모든 검색 조건의 정당성까지 결론내리지 않는다. 소실된 중간 배열은 실제 보존 자료와 source 기반 추론을 구분한다.

## partition 입력의 소비 전제

ACE partition 수나 지역 입력을 바꿀 때는 양수 검사·compile 성공만으로 유효성을 판단하지 않는다. 실제 label 개수·거리 행렬 shape·최소 및 차순위 선택이 요구하는 열 수를 확인한다. 여러 partition을 직접 참조하는 구현에서 단일 partition을 전역 대조로 사용하지 않는다.

바깥 index 설정과 지역 builder의 유효 설정을 구분한다. 지역 크기·factory·heuristic에 따라 차수·중간 후보 폭·builder가 달라질 수 있다. factory 결과와 실제 graph extent를 각각 확인하고, core 행 복사 폭·stride와 대조한다. 설정식만으로 실제 후보 품질을 검증했다고 하지 않는다.

같은 행 수의 prefix는 실제 cluster 배정으로 만든 지역 입력과 다르다. 필요한 지역 배열·ID 대응·query의 지역 순번을 보존해 작은 루프에서 재사용한다. 대체 입력의 초기값 잔류나 miss를 원 실행의 원인으로 옮기기 전에 실제 지역 입력에서도 관측되는지 확인한다.

유효 ID와 차수만으로 후보 거리의 갱신을 보장하지 않는다. 무작위 유효 ID에 거리 초기값을 붙이는 구현인지 확인하고, 가능하면 공개 API의 거리와 초기값 잔량을 대조한다. invalid-ID 대체와 유효 ID의 초기값 잔류를 구분한다. 출력만으로 내부 거리 평가가 전혀 없었다고 단정하지 않는다.

반환 옵션·graph 소유 방식·반복량·정밀도를 바꾼 생성은 별도 대조다. 해당 버전의 enum과 실제 버퍼를 확인하며 AUTO를 FP32 강제로 읽지 않는다. half 저장·float 누산·float 반환을 구분한다. kernel 경로도 달라지면 효과를 저장 정밀도 하나로 설명하지 않는다. 거리 오차·후보 포함률·검색 개선·회귀를 따로 기록한다.

정적 입력 계약 문제는 원 실행이 그 조건을 사용했을 때만 원인 후보가 된다. 관측한 반복량이나 설정을 보편 수정값으로 채택하지 않는다.

## 계측 한도와 자원 거부

allocator·recorder를 붙인 실행이 OOM이나 할당 실패로 끝나면 첫 거부의 이유·요청 bytes·live bytes·cap·phase·기록 수를 보존한다. 잘못된 설정·레코드 포화·로그 I/O 실패도 backend 호출 전에 같은 예외를 만들 수 있다. SDK 예외와 계측기 거부, 정리 결과를 구분한다.

자세한 성공 로그와 실제 할당 제한을 분리한다. 고정 크기 phase별 counter·high-water·첫 거부 요약을 사용할 수 있지만 pointer·pending ledger의 용량과 포화 이유도 남긴다. CPU 요약의 정보 보존은 새 recorder의 실제 동작이나 큰 형상 지원의 증거가 아니다.

자원 정책과 알고리즘 변경은 따로 기록한다. allocator·timeout·파일 cap은 수학을 바꾸지 않아도 실행 거부와 I/O 지연을 만든다. tracking 밖의 managed·driver·runtime 할당을 미관측으로 남긴다. training·persistent index·query scratch·caller 생존량을 함께 모델링하고, 관측 counter의 high-water를 전체 물리 peak로 표시하지 않는다.

파일 cap은 개별 파일 한도인지 전체 합계인지 구분한다. 삭제된 이름의 열린 FD도 디스크를 점유할 수 있다. 디렉터리 stat·모델 reserve·실제 할당량을 구분하며 회수·압축·로그·정리 시간도 선언한 범위에 포함한다. 지원 불가로 끝낼 때는 가장 좁은 미확정 항목과 부족한 근거를 남긴다.

## 합성 입력의 수치 범위와 출력 sentinel

큰 source ID를 좌표로 인코딩하면 원 데이터의 norm·거리 분포를 바꿀 수 있다. 유한한 입력이라는 이유로 FP16 등 중간 거리·LUT·coarse score가 표현 범위에 든다고 가정하지 않는다.

실패 입력·설정·출력 bytes를 먼저 보존한다. fixture를 보완하면 입력의 한 축만 바꾸고 다른 바이트·ID 대응의 보존을 확인한다. 새 fixture의 성공은 이전 실패의 책임을 닫지 않는다.

범위 밖 출력이 sentinel과 같다면 실제 반환인지, self·중복 처리 뒤 writer가 일부 칸을 쓰지 않은 것인지 구별한다. 잘못된 ID를 채우거나 필터링해 합격시키지 않는다. 내부 최초 차이를 관측하지 않았다면 입력 스케일 민감성과 특정 SDK 결함을 분리해 보고한다.

## 저장 graph의 lazy 대조 비용

후보 생성 시 adjacency까지 읽으면 확장되지 않을 후보의 행도 계산할 수 있다. 원 탐색기의 간선 읽기 시점에 맞춰 필요한 행을 lazy 생성하고, 고정 graph에서 기존 구현과 이벤트·retained·종료·평가 수가 같은지 확인한다.

생성 거리 계산과 검색 거리 계산, 논리적 corpus 읽기와 물리 I/O를 분리한다. 중단된 행의 소비량도 비용에 포함하되 미완료 행은 결과로 사용하지 않는다. 행 선택 규칙은 query의 expected ID와 독립적으로 고정한다.

부분집합 순서는 shape와 별개다. 원 ordinal·level·외부 ID와 부분집합 순열을 보존하고, 저장 page 순서와 생성 입력 순서가 같은지 source에서 확인한다. 간선 slot 보존만으로 과거 실행 branch·정밀도·비결정적 생성을 복원하지 않는다.

새 방문으로 cache가 부족하면 선언한 예산 안에서 확장하거나 partial로 끝낸다. 원 간선 fallback·가짜 거리로 완결시키지 않는다. 일부 query의 회복과 전체 paired 완료를 분리하고, 실패·정리·기록 오류 뒤 cache hit로 부분 자료를 다시 소비하지 않는다. 실패한 한 실험 정책을 모든 graph 설계의 불가능성으로 확대하지 않는다.

## 같은 native 파일 보존

임시 직렬화 FD가 사라져 같은 산출물의 native·PG 대조가 막혔을 때 적용한다. 기존 보존 지점을 먼저 찾고, hook이 필요하면 소유 backend·DataDir·입력 형상·원 FD identity로 범위를 제한한다. 생성·검색의 수학·seed·정책은 그대로 둔다.

close 전 pread 복사는 가능한 방법이며 모든 환경의 interposition 동작을 보장하지 않는다. 원 FD 위치·flags·close 결과·errno를 유지하고 복사 byte·시간·메모리 한도와 부분 파일을 기록한다. 원 close를 재시도하거나 capture 오류로 원 제품 결과를 바꾸지 않는다. 복사 지연도 실제 비용으로 계상한다.

생산자 FD의 identity·size와 복사본 내용 지문을 결속하고, 실제 사용한 TID sidecar의 CRC와 heap TID→source ID를 연결한다. native label을 source ID로 가정하지 않는다. TID 파일은 R/W로 열렸어도 producer가 쓰기를 마친 뒤 논리적으로 불변일 수 있으므로 모드만으로 판정하지 않는다.

FINALLY는 오류에도 실행된다. capture 완료·파일 형식 유효·PG CREATE/commit 성공을 별도 조건으로 확인한다. copy·CRC·기록 실패는 부분본과 첫 오류를 보존하고 후속 소비를 거부한다. 재귀 close나 여러 같은 형상의 build에서 다른 산출물을 잘못 묶지 않는다.

SDK 공개 검색과 PG는 같은 보존 파일·query 바이트·metric·검색 조건으로 연결한다. converter 대조는 vector·의미상 label·entry·level·active 간선 순서를 포함한다. 별도 생성 결과의 차이를 변환 손실로 부르지 않는다. 같은 graph여도 검색 구현의 tie·수치 의미는 별도 검증 대상이다.

회수 오류와 실제 실행 오류를 분리한다. 부분 회수본과 remote 산출물을 보존하며 이미 완료한 workload를 단순 회수 실패 때문에 다시 실행하지 않는다. 회수가 전체 선언 시간창 밖에서 끝났으면 그 조건의 미충족을 남기고 시계를 다시 시작하지 않는다.

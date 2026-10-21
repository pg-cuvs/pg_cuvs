---
name: pg-cuvs-investigate
description: pg_cuvs의 검색 오답과 native·직렬화·PostgreSQL 경계의 결과 불일치를 가설 구별 실험으로 조사하고 원인에 연결된 수정을 검증한다. 일상 서버 운영이나 저장소 전체 품질 감사에는 사용하지 않는다.
---

# pg_cuvs 정확성 조사

## 실패를 설명하는 실험

현재 합격 조건, 관측된 실패, 남은 가설을 짧게 적고 이를 구별하는 가장 작은 실험을 선택한다. 기존 source, 실패 원문, 고정 산출물을 먼저 활용한다. 조사 계획이나 문서 정비를 별도 승인 절차로 만들지 않는다. 실행은 현재 사용자가 허용한 범위 안에서 진행한다.

비교 전에 다음 정보를 보존한다.

- query의 ordinal, expected ID, 입력 바이트 또는 재현 규칙과 내용 지문
- 반환 ID와 거리, 거리 단위, 실제 검색 설정
- 실행 코드·라이브러리·입력·산출물의 버전과 처음 실패한 경계
- 부분 결과와 원 예외, 별도로 구분한 기록·정리 오류

이미 사라진 과거 값은 사후 추론과 관측 복구를 구분한다. expected 값이나 원 합격 조건을 바꾸어 실패를 통과시키지 않는다. 탐색 폭이나 graph를 바꾼 성공은 대조 실험이며, 수정의 적합성은 원 조건에서 확인한다.

같은 고정 query를 비교할 때 총 miss 수와 query별 개선·회귀를 함께 본다. 실패 총수가 같아도 대상은 달라질 수 있다. 성공 query가 없는 기준선으로 성공 사례 보존을 주장하지 않는다. 같은 검색 설정의 비교와 같은 거리 평가·확장·시간 비용의 비교를 구분한다.

같은 seed·shape·dtype만으로 생성기 버전이나 실행환경이 다른 입력의 바이트 동일성을 가정하지 않는다. 고정 입력이면 실행 전에 해당 범위의 내용 지문을 비교한다. 일부 sample 일치를 전체 입력 복구의 증거로 쓰지 않는다.

## 의심 경계와 최소 비교

아래 항목은 가설에 따라 선택한다. 매번 전부 실행하는 고정 suite가 아니다.

| 의심 경계 | 가장 작은 유효 비교 |
|---|---|
| 입력·거리 | query 바이트, 차원·dtype·정규화, L2와 squared L2, 실제 거리 함수 |
| ID 연결 | ordinal·native node·label·실제 heap TID·source ID의 대응 |
| graph·검색 | 같은 저장 graph와 query를 고정한 탐색 또는 간선 대조 |
| 직렬화·반입 | 실제 production converter와 독립 decoder의 vector·label·edge·entry·level 비교 |
| PG 검색 경로 | 실제 query plan·GUC·거리 support 함수·선택 index와 반환값 |

도달성과 payload 보존은 유한 폭 검색의 정확성을 보장하지 않는다. 반대로 한 번의 검색 miss로 graph 단절이나 생성 결함을 확정하지 않는다. 평가·수용·확장·탈락·종료 시점의 상태를 구분한다. 최종 bound를 탐색 내내 고정된 장벽처럼 사용하지 않는다.

필요한 경우에만 다음 상세를 읽는다.

- 설정을 바꿔도 결과가 같거나 검색을 재생할 때: [설정 소비와 검색 재생](references/lessons.md#설정-소비와-검색-재생)
- 같은 fork의 caller를 독립 구현과 혼동할 우려가 있을 때: [고정 graph의 독립 검색 대조](references/lessons.md#고정-graph의-독립-검색-대조)
- ACE의 partition이나 지역 입력을 바꿀 때: [partition 입력의 소비 전제](references/lessons.md#partition-입력의-소비-전제)
- 큰 graph의 일부 행을 lazy 대조할 때: [저장 graph의 lazy 대조 비용](references/lessons.md#저장-graph의-lazy-대조-비용)
- 임시 출력 FD를 보존해 native와 PG를 비교할 때: [같은 native 파일 보존](references/lessons.md#같은-native-파일-보존)

production 호출자를 작은 입력에 직접 연결한다. stub·합성 identity·재구성한 입력을 쓰면 그 증명 한계를 명시한다. data-only 조건과 실제 소비자의 하드코딩이 충돌하면 구체적인 소비 계약을 보여 범위를 정정한다. 이를 우회하는 별도 프레임워크를 기본값으로 만들지 않는다.

## 실패를 숨기지 않는 관측

- 원문과 부분 결과를 저장한 뒤 검증한다. 준비·실행·fetch·전송·기록의 어느 단계에서 실패해도 첫 원인을 유지한다. 기록 확인에 실패한 결과를 소비하며 다음 query로 진행하지 않는다.
- cached 데이터가 있어도 이번 읽기의 missing·replaced·partial·pending·deadline 실패를 성공으로 바꾸지 않는다. 실패한 cache나 미검증 부분 결과는 후속 비교에 사용하지 않는다.
- rollback 실패가 close 시도를 막지 않게 하고 cleanup 오류가 원 오류를 덮지 않게 한다. 한도는 수집 뒤가 아니라 reader·iterator·writer가 소비하는 동안 검사한다.
- 준비·compile·fixture·CPU 요약·배포·실제 호출·검색 품질을 구분한다. 각 단계의 PASS를 다음 단계나 전체 시스템의 PASS로 확대하지 않는다.
- 테스트 이름 대신 실제 주입 인자·실행 분기·assert를 확인한다. fixture 오류와 계측기 거부를 제품 결함으로 분류하기 전에 구별한다.

해당 문제가 있을 때 [계측 한도와 자원 거부](references/lessons.md#계측-한도와-자원-거부), [합성 입력의 수치 범위와 출력 sentinel](references/lessons.md#합성-입력의-수치-범위와-출력-sentinel), [RPC timeout 뒤 늦은 응답과 최초 오류](references/lessons.md#rpc-timeout-뒤-늦은-응답과-최초-오류), [진단 frame과 ACK frame의 서로 다른 한도](references/lessons.md#진단-frame과-ack-frame의-서로-다른-한도)를 읽는다.

## 자원 정책과 관측 계약

형상·상수식에 따른 용량 추정, 정책이 허용한 실행 범위, 실제 할당·파일 생존량을 구분한다. 작은 실행의 peak나 동시 pointer 수를 큰 형상의 상한으로 대입하지 않는다. query batch만 줄여 training·persistent index·search scratch의 전체 동시 생존량을 지원한다고 판정하지 않는다.

자원 cap·timeout·recorder·observer 변경은 알고리즘·정밀도·seed·입력 변경과 분리한다. 자원 정책의 거부는 원문을 보존한 지원 불가 결과가 될 수 있다. 계측 변경만으로 수학이 보존됐다고 주장하거나, 별도 입력·설정의 재실행을 같은 실행의 계속으로 취급하지 않는다.

관련 경계에만 관측 계약을 적용한다.

- mutation 대상의 guard와 이미 로드된 불변 DSO의 읽기 계약을 구분한다. 같은 FD의 streamed hash와 전후 identity·size·metadata를 확인하되, 모든 소비자에 같은 hardlink 정책을 요구하지 않는다.
- 실제 project 모듈의 기대 이름·경로·내용 지문을 먼저 결속하고 missing·permission·wrong identity를 구분한다. 출처 불명의 런타임 객체를 파일 모듈로 꾸미거나 whitelist만으로 provenance를 만들지 않는다.
- 관련 프로세스의 시점별 관측, 소유 child의 종료, PG clean-control 증거를 구분한다. 반복 snapshot이나 parent 종료만으로 atomic·global closure를 주장하지 않는다.
- 로컬 계정 해석, DNS, 접속 도구, host-key 확인, 원격 서비스 상태를 별개로 진단한다. 로컬 전송 실패를 서버 중단으로 해석하거나 인증·보안 설정을 임의 우회하지 않는다.
- PG 재시작 후 과거 receipt를 재사용할 때는 [과거 integrity 증거의 역할](references/lessons.md#pg-재시작-뒤-과거-integrity-증거의-역할)을 읽는다.
- relation 파일과 engine buffer의 시점을 구분한다. UNLOGGED 파일의 미반영 page를 converter 손상으로 단정하지 않는다. [UNLOGGED page와 shared buffer](references/lessons.md#unlogged-page와-shared-buffer)를 참고한다.

## 수정과 검증

원 실패를 재현하고 원인과 연결된 변경만 검증한다. 작은 대조로 가설이 갈리면 그 경계를 수정하며, 새 대형 실행을 기본값으로 삼지 않는다. 변경한 translation unit·dependency·link delta가 정당화하면 기존 object를 재사용한다. 직접 영향이나 필수 통합 gate 없이 SDK 전체 재빌드나 변경 없는 suite를 반복하지 않는다.

협업은 독립 가설과 코드 경계처럼 서로 다른 예상 결과를 낼 수 있는 단위로 나눈다. 단순 전사·봉인·미시 승인으로 과분할하지 않는다. 현재 허용된 작업은 끝까지 구체화하고, 중요한 방향·비용 변화와 해결되지 않은 외부 차단을 보고한다. 절차와 보고서 수를 원인 규명의 성과로 대신하지 않는다.

## 재사용 지식 관리

skill을 작성·정리할 때 [관리 기준](references/maintenance.md)을 읽는다. 핵심 판단은 본문에, 조건부 상세는 reference에, 특정 실행의 원문·수치·식별자는 별도 실행 기록에 둔다. 새로 재사용할 지식이 없으면 형식적으로 갱신하지 않는다.

한 내용의 정본은 한곳에 유지한다. skill 갱신은 현재 수락 조건이나 사용자 권한을 바꾸지 않는다. 문서 정비 때문에 새 승인·정기 보고·별도 운영 프레임워크를 추가하지 않는다.

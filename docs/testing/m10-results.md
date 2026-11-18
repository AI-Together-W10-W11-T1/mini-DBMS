# M10 엔진 인덱스 통합 결과

2026-11-18 수요일(Asia/Seoul). 구현·독립 QA와 사용자 완료 승인·정상 하네스 DONE을 확인했다. 품질 반려0회다.

## 범위와 선택

사용자 1A/2A로 DELETE는 압축한 행과 교체 인덱스를 사전 준비하고 UPDATE는 ID·행 순서를 보존해 기존 인덱스를 재사용한다. INSERT는 배치 노드 풀을 먼저 예약하고 디스크 반영 뒤 할당 없이 공개한다. 이전 선택인 현재 존재하는 최대 ID+1·빈 테이블1·ID→메모리 행 번호를 연결했다. 선택·계획 원본은 [계약](../../.cache/m10/contract.json)과 [Planner](../../.cache/m10/planner-result.json)에 있다.

시작 시 저장된 모든 행을 검증하며 테이블별 인덱스를 만든다. INSERT는 배치 전체의 값·중복·범위를 검사하고 기존 인덱스 조회와 배치 ID 정렬로 중복을 확인한다. 예약량은 검사된 분할 경로 상한과 전체 노드 상한을 사용한다. DELETE의 준비 실패는 기존 행·인덱스를 보존하고 반영 뒤 정리 실패는 반영 성공·건수를 유지한다.

단일 테이블의 ID 등호·역방향 등호·수식 참조·AND 필수 등호에서 후보를 찾고 전체 조건을 다시 평가한다. OR는 선형 검색한다. SELECT/VALUES/UNION의 전체 정적 검증을 메타데이터 전용 선행 단계에서 수행한 뒤 인덱스에 접근한다. 성공 쿼리는 두 번 파싱하며 이 추가 비용은 미측정이다.

## 인수 검증

각 행은 Given 조건에서 When 실행 후 Then 결과를 실제 검사했다. 원문은 [CLI 시험](../../tests/m10_cli.py)과 [native 시험](../../tests/unit_m10.c)이다.

| ID | Given | When | Then |
|---|---|---|---|
| M10-STARTUP-01 | 정상 저장·후반 중복·할당 실패 | 열기 | 정확 매핑·손상/자원 진단·부분 정리 |
| M10-AUTO-01 | 빈 테이블·중간 위치 ID 컬럼 | 자동/명시 복수 INSERT | VALUES 순서·명시 ID 보존 |
| M10-AUTO-02 | 독립 테이블·최대 ID 삭제 | 자동 INSERT·재개방 | 테이블별 현재 최대+1·삭제 최대 재사용 |
| M10-AUTO-03 | INT64 상한 부근 | 복수 자동 발급 | INTEGER_RANGE·문장 전체 보존 |
| M10-INSERT-01 | 기존/배치 중복·후반 타입 오류 | 복수 INSERT | 오류 시 저장·발급 효과 없음 |
| M10-RESERVE-01 | 기존2000키·추가2000키 | 예약 실패 열거·취소·적용 | 기존 트리/노드 수 보존·적용 할당0·4000키 값 일치 |
| M10-LOOKUP-01 | 존재/부재 ID | 정방향·역방향·수식 조회 | 실제 인덱스 접근·후보 행만 방문 |
| M10-LOOKUP-02 | AND·NULL·충돌 등호 | 후보 재평가 | 전체 조건·UNKNOWN 의미 보존 |
| M10-LOOKUP-03 | OR·부재 후보·정적 오류 | 조회 | OR 선형·정적 오류 선행·오류 시 인덱스/행 접근0 |
| M10-UPDATE-01 | 비정렬 ID·기존 인덱스 | 비ID UPDATE | 수정값 조회·기존 트리 재사용 |
| M10-DELETE-01 | 여러 행·전체 삭제 | 압축·재삽입 | 행 번호 재매핑·빈 테이블 자동 ID1 |
| M10-DELETE-02 | 준비/교체/정리 실패 | DELETE | 반영 전 바이트/매핑 보존·반영 후 성공 유지 |
| M10-STORAGE-01 | 예약/append/정리 실패 | INSERT·재시도 | 반영 전 보존·반영 후 매핑/건수 유지 |
| M10-RESTART-01 | 최대 삭제·저장된 행 | 별도 프로세스 INSERT | 현재 최대 규칙 재구성 |
| M10-MIGRATE-01 | ID 대소문자·컬럼 재배열 | migrate·재개방·INSERT | ID 보존·인덱스 재구성·자동 ID/조회 일치 |

## 실제 검사와 실패

- 고위험 실제 Red는 configure·compile 성공 뒤 AUTO-01·RESERVE-01·LOOKUP-01·DELETE-02 실패였다. 초기 RESERVE는 없는 API를 나타내는 빌드 가능한 assertion stub이었으며 예약 원자성 전체의 Red 증거에는 한계가 있다. 최종 Green은 실제 prepare/apply의 할당 실패·취소·적용 무할당·전체 키/값을 검사했다.
- 첫 부분 검사는4/6이었다. 새 native helper가 SQL 오류의 공개 MD_NEXT_STOP을 허용하도록 수정했다. 첫 전체 회귀149/150의 실패는 새 DELETE 시험에 남은 allocator fail-at 상태였으며 초기화해 해결했다. 메타데이터 선행 검사의12/13 실패는 기존 INVALID_ORDER_BY와 다른 새 기대값을 정렬해 해결했다.
- 최종 작성자 전체 회귀150/150·skip0, 독립 Reviewer_QA의 새 configure/build/전체 회귀150/150·skip0와 독립 CLI26개가 통과했다. 독립 검토 지적은 없다. 검사·실패·기술 입력 근거는 [작성자 JSON](../../.cache/m10/implementer-result.json)과 [독립 QA JSON](../../.cache/m10/reviewer-result.json)을 따른다.
- milestone_cli.py와 m2_cli.py의 기존 ID 생략 음성 기대2개를 격리된 자동 ID 양성 fixture로 정렬했다. 명시 ID의 NULL·타입·범위·중복 검사는 보존했으며 독립 QA가 정렬 범위를 확인했다. 현재 Phase 2 확장과 역사적 Phase 1 ID_REQUIRED 설명은 [SQL 계약](../sql-contract.md)에 함께 남겼다.
- 승인 문서의 원문 대조·UTF-8·로컬 링크·보호 입력 SHA와 Wiki 일괄 등록·정상 finish의 실제 관측은 [Knowledge JSON](../../.cache/m10/knowledge-result.json)을 따른다. 기계 시간·로그·해시는 연결된 JSON에 둔다.

## 한계와 다음 행동

100만 건 생성·삽입·인덱스/선형 성능 비교, 메타데이터 선행 단계의 지연 측정, sanitizer·누수 분석은 미실행이다. Phase 1 전체204개 감사는 사용자 지시로 생략했다. 전원 손실·강제 종료 복구·전수 OS fault는 검증 범위 밖이며 기존 M7 무정렬 ALL 순서 관찰도 보존한다. DELETE는 교체 인덱스를 전부 준비한다. JOIN·범위 조건·일반 OR는 기존 검색 방식을 사용한다.

M10 사용자 완료 승인·정상 하네스 DONE을 확인했다. 다음 증분의 100만 건 생성·정확성 검증·측정 조건을 협의한다. 새 Goal은 생성하지 않았다. 재개 상태는 [HANDOFF](../../HANDOFF.md)를 따른다.

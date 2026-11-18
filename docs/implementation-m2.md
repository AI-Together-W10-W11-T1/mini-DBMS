# Phase 1 M2 구현 계획

2026-11-17 화요일(Asia/Seoul). 사용자는 다음 증분 선택에서 **1-A 전체 기능, 2-A 단일 테이블 이름 처리 포함, 3-B M2 전체 구현 뒤 사용자 검토 한 번**을 선택했다. Planner 제안과 Reviewer_QA의 설계 검토를 반영한 구현 계획이다. 후속 진행 지시로 구현을 수행했고 독립 QA를 통과했다. 사용자가 M2 완료를 승인했고 정상 하네스 CLI로 DONE을 확인했다. 새 Goal은 생성하지 않았다. 실제 명령·시험·한계는 [M2 결과](testing/m2-results.md)에 기록한다.

의미의 원본은 [SQL 계약](sql-contract.md), [스키마 계약](schema-contract.md), [저장 계약](storage-contract.md), [CLI 계약](cli-contract.md)이다. 기존 [M1 결과](testing/m1-results.md)와 완료한 M1 Goal은 다음 구현의 범위를 대신 정하지 않는다.

## 선택한 범위

| 기능 | M2 동작 |
|---|---|
| 복수 행 INSERT | 필수 컬럼 목록, 리터럴·NULL 행 목록, 생략 비ID 컬럼 NULL, 양의 고유 ID, 정확한 affected_rows |
| INSERT 문장 원자성 | 전체 행의 이름·개수·타입·정수 범위·기존/문장 내부 ID 중복을 검사하고 한 문장 단위로 추가·복원 |
| 확장 WHERE | `= <> < <= > >=`, `IS NULL`, `IS NOT NULL`, `AND`, `OR`, 괄호와 컬럼/리터럴 양쪽 조합 |
| 단일 테이블 이름 처리 | `users.id`, `FROM users AS u`, `u.id`, 출력 컬럼의 명시적 `AS` |
| 독립 VALUES | 저장 없이 결과 행 생성, 위치별 타입 추론, 전부 NULL인 결과 열, column1부터 시작하는 출력 이름 |

후속 구현은 UPDATE/DELETE·계산, 정렬·페이지, JOIN, 집계, UNION 및 migrate 순서의 기존 계획을 따른다. Phase 2·3의 상세 설계는 해당 단계에서 진행한다.

## 구현 순서와 책임

1. 복수 행 INSERT의 실제 실패 시험을 추가하고 성공하는 빌드에서 기능 Red를 확인한다. 행 목록 전체를 준비한 뒤 한 문장의 append·flush·복원 경계로 저장을 확장한다.
2. WHERE를 파싱·이름 해석·타입 검사한 뒤 행별로 평가한다. 같은 구간에서 단일 테이블 별칭·수식 컬럼·출력 AS를 연결한다.
3. 독립 VALUES의 행 목록·타입 추론·이벤트 결과를 연결하고 표/JSONL 출력을 검증한다.
4. 누적 회귀, 독립 코드 검토·실행 QA, 지식 기록과 마감 검사를 수행하고 M2 전체 결과를 사용자에게 한 번 보고한다.

각 구간은 계약/Given/When/Then → 실제 기능 Red → 최소 Green → Refactor → 회귀를 따른다. 내부 단계별 검증은 계속 수행하고 사용자 검토는 전체 M2 결과에서 받는다. 새 계약 선택이나 범위 변경이 생기면 Master가 사용자와 상의한다.

정적 엔진과 얇은 CLI의 engine/sql/catalog/storage/cli 다섯 책임을 바탕으로 구현한다. 현재 변경 중심은 `src/sql.c`, `src/storage.c`, 필요한 내부 선언, 기존 단위·CLI 시험 및 CTest 등록이다. 실제 변경 파일·시험 이름은 착수 시 구현 계약에 고정한다.

- sql은 행 목록·조건·단일 입력의 이름·출력 이름을 검증한다. 모든 조건 가지를 실행 전에 검사하므로 빈 테이블이나 거짓 조건도 없는 컬럼·타입 오류를 숨기지 않는다.
- 구현 전 `md_storage_insert`는 한 행을 반영했다. 복수 행에서는 전체 준비·총 길이 및 할당 크기 검사·성공 결과 준비 후 원래 파일 길이 하나를 기준으로 append한다. 행마다 기존 함수를 호출해 앞 행을 먼저 확정하지 않는다.
- engine의 요청·이벤트 수명을 유지한다. 결과 값과 컬럼 이름은 이벤트가 소유하고 DB·요청 종료 뒤에도 유효해야 한다. 실패 경로는 준비 중인 모든 메모리를 해제한다.
- catalog는 기존 스키마 규칙을 제공한다. M1 데이터 파일과 스키마를 그대로 열고 M2로 추가한 레코드를 같은 바이너리 포맷으로 읽을 수 있어야 한다.
- cli는 기존 필수 경로·파일/명시적 stdin·표/JSONL·진단·종료 코드를 적용한다. 독립 VALUES도 기존 run 준비 검증을 거친다. `MdType`의 `MD_NULL`과 기존 결과 메타데이터를 사용해 NULL 전용 결과 열을 표현한다.

## 반드시 지킬 동작

조건 우선순위는 괄호 → 비교/NULL 검사 → AND → OR다. NULL 비교는 UNKNOWN이며 WHERE는 TRUE만 통과한다. SQL 계약의 3값 진리표를 대칭 조합까지 검사한다. INT64와 TEXT는 암묵적으로 변환하지 않고 TEXT는 UTF-8 바이트 순서로 비교한다.

입력 별칭을 붙이면 그 별칭으로만 수식한다. `FROM users AS u` 뒤 `users.id`는 UNKNOWN_TABLE이다. 출력 AS는 WHERE의 입력 이름으로 등록하지 않는다. 단일 테이블 기본 출력 이름은 스키마 선언 원철자의 비수식 컬럼명이다. 최종 출력 이름은 ASCII 대소문자 무시 기준으로 유일해야 하며, `id AS a,id AS b`는 허용하고 `id AS x,name AS X`는 DUPLICATE_OUTPUT_NAME이다.

INSERT는 원본 변경 전 전체 검증·직렬화·메모리·건수 준비를 마친다. 전체 append와 FlushFileBuffers 성공이 반영 경계다. 그 전 실패는 원래 길이로 복원하고 flush·길이 확인을 수행한다. 복원 실패는 파일·진행 표식을 보존하고 세션·재시작을 차단한다. 반영 뒤 cleanup·stdout 실패에서도 삽입 행 전체의 성공과 건수를 보존하며 후속 SQL을 중단한다. 종료 코드 우선순위는 기존 CLI 계약을 따른다.

독립 VALUES는 행별 컬럼 수와 위치별 비NULL 타입을 모두 검사한 뒤 결과를 공개한다. 같은 위치의 비NULL 값으로 타입을 추론하며 전부 NULL이면 결과 전용 NULL 메타데이터를 사용한다. 후반 행 오류에서 앞 결과 행을 부분 성공으로 출력하지 않는다.

## Given/When/Then 검증 계획

아래 시험은 준비된 `users(id INT64,name TEXT,age INT64,team TEXT)`를 사용한다. 정렬 없는 결과는 행 집합과 중복 개수로 비교한다. 아래는 설계 시 정한 검증 기준이며 실행 결과는 [M2 결과](testing/m2-results.md)와 실제 시험 원문을 따른다.

| ID | Given | When | Then |
|---|---|---|---|
| M2-INSERT-01 | 빈 users | `INSERT INTO users(id,name) VALUES(5,'Kim'),(6,'Lee');` | 2행 반영, affected_rows=2, age/team NULL. 별도 프로세스 재조회와 바이너리 해독 결과 일치 |
| M2-INSERT-02 | 빈 users 또는 기존 id=5 | 문장 내부 중복 ID 또는 후반 행의 기존 ID 충돌 | DUPLICATE_ID, 해당 문장의 모든 행 미삽입, 기존 파일 바이트 보존 |
| M2-INSERT-03 | 저장 전 스냅샷 | 후반 행의 개수·타입·정수 범위 오류 | 해당 오류 코드, 전체 미반영. INT64 양끝 두 행은 성공 |
| M2-INSERT-04 | 기존 유효 데이터 | 복수 행 일부 바이트를 쓴 뒤 실패 주입 | 원래 길이·바이트 복원과 재시작 조회 확인. 복원 실패 변형은 표식 보존·exit 6·재시작 차단 |
| M2-INSERT-05 | 복수 행 반영 성공 | cleanup·stdout 실패 및 두 실패 동반 | 전체 성공 건수·반영 유지, 후속 문장 미실행, 기존 종료 우선순위 |
| M2-WHERE-01 | NULL과 비NULL INT64/TEXT 행 | 6종 비교, `1=id`, `age=age`, 상수 비교 | 타입별 기대 결과, NULL 비교 UNKNOWN, TEXT 대소문자·빈 문자열·공백 구분 |
| M2-WHERE-02 | 진리표 조합 fixture | AND/OR·괄호·IS NULL/IS NOT NULL | 3값 진리표와 우선순위 일치, TRUE만 반환 |
| M2-WHERE-03 | 빈 users 및 정상 users | `SELECT id FROM users WHERE 1=0 AND age='20';` 또는 없는 컬럼 조건 | TYPE_MISMATCH 또는 UNKNOWN_COLUMN. 행 수·조건의 실행 여부에 의존하지 않음 |
| M2-NAME-01 | 기본 users | `SELECT u.id AS user_id,u.name AS user_name FROM users AS u WHERE u.id=1;` | 별칭·수식 참조 성공, 선택한 출력 이름과 타입 유지 |
| M2-NAME-02 | 기본 users | 원래 테이블 수식자 사용·출력 AS만 WHERE에서 참조·출력 이름 중복 | 각각 UNKNOWN_TABLE·UNKNOWN_COLUMN·DUPLICATE_OUTPUT_NAME. 서로 다른 AS의 중복 입력 컬럼 출력은 성공 |
| M2-VALUES-01 | 준비된 DB와 저장 전 스냅샷 | `VALUES(1,'Kim'),(2,'Lee');` | column1/column2, INT64/TEXT 2행. DB 파일과 ID 상태 불변 |
| M2-VALUES-02 | 준비된 DB | `VALUES(NULL),(1);` 및 `VALUES(NULL),(NULL);` | 각각 INT64·NULL 전용 열 메타데이터. table/JSONL 값·타입 일치 |
| M2-VALUES-03 | 저장 전 스냅샷 | `VALUES(1,'Kim'),(2);` 및 `VALUES(1,'Kim'),(2,3);` | 각각 COLUMN_COUNT_MISMATCH·TYPE_MISMATCH, query_result 미공개·파일 불변 |
| M2-REGRESSION-01 | M1 작성 파일·기존 16개 시험 | M2 실행 파일로 재시작 조회·기존 시험·파일/stdin 및 table/JSONL 조합 실행 | 기존 의미와 파일 호환 유지, 새 입력의 문장별 진행·오류 중단·이전 반영 유지 |

[기존 SQL 인수 명세](testing/phase1-sql-acceptance.md)의 P1-SQL-005/013/014/029/030/031/032/052/053/054는 각 원문 전체 입력·변형을 실행해 대응한다. P1-SQL-017/027/028/033/034/043은 UPDATE·정렬·JOIN을 포함한 복합 사례이므로 이번에 실행한 부분만 기록한다. 관련 저장·CLI 인수 명세도 실제 assertions와 대응하며 전체 204개 완료로 확대하지 않는다.

## 완료 조건과 인수

실제 C 빌드 뒤 기능 Red를 확인하고 Green·누적 회귀를 수행한다. M1 16개 회귀와 위 M2 시험을 등록하고 실패·누락·skip을 성공으로 처리하지 않는다. Reviewer_QA는 고정 소스에서 새 빌드·시험을 실행하며 검토와 실행 QA 결과를 나누어 보고한다. 운영 실행 파일이 시험용 실패 주입을 무시하는 것도 유지한다.

Master는 승인 범위로 Implementer를 배정하고, 독립 Reviewer_QA 결과를 인수한 뒤 Knowledge Manager에게 검증한 사실·실패·한계를 기록하게 한다. Wiki 원문 대조와 실제 `python scripts/dev_tools.py finish`를 수행한다. 사용자 검토는 M2 전체 결과에서 한 번 받으며 최종 확인 뒤 해당 구현 작업을 완료한다. 중간 품질 반려와 세 번째 반려 보고 규칙은 기존 하네스 계약을 따른다.

이번 문서는 승인한 계획과 선택의 기록이다. 실제 C 구현·30개 회귀·독립 추가 104개 검사를 수행했고 결과는 별도 기록했다. 성능 측정은 미실행이다. 현재 시작 시 이차 시간 ID 검사·선형 조회와 전원 차단 복구 검증 한계는 [M1 결과](testing/m1-results.md)에 남아 있다.

# Phase 1 M3 구현 계획과 불변식

2026-11-17 화요일(Asia/Seoul). 사용자는 **A — UPDATE·DELETE·SET 계산·파일 교체 실패 검증 전체를 구현하고 최종 결과를 한 번 확인**하는 방식을 선택했다. Planner의 구체 계획에 따라 구현·독립 QA를 수행했고 사용자가 M3 완료를 승인했다. Master가 정상 하네스 CLI로 DONE을 확인했다. M3 새 Goal은 생성하지 않았다. 실제 결과는 [M3 검증 기록](testing/m3-results.md)을 따른다.

의미의 원본은 [SQL](sql-contract.md), [스키마](schema-contract.md), [저장](storage-contract.md), [CLI](cli-contract.md) 계약이다. 이번 선택은 기존 문법을 구현하는 단위와 확인 시점이며 새 SQL 의미를 추가하지 않았다.

## 선택과 구현 경계

| 방식 | 장점 | 비용과 선택 |
|---|---|---|
| UPDATE·DELETE·계산·교체 검증 전체 | 공통 저장 경로와 문장 원자성을 함께 검증 | 한 번에 검토할 결과가 많음. 사용자 A 선택 |
| DELETE·교체 먼저, 이후 UPDATE·계산 | 작은 단위로 확인 | 계획·검토·기록을 두 번 진행. 미선택 |

engine/sql/catalog/storage/cli 다섯 책임과 공개 API·바이너리 포맷1·CLI 규약을 유지했다. 변경 파일은 `src/sql.c`, `src/storage.c`, `src/internal.h`, `tests/m3_cli.py`, `tests/unit_milestone.c`, `CMakeLists.txt`, `scripts/c_checks.py`다. 소스 수정과 독립 검토·검사를 겹치지 않았다.

1. 기존 30개에 M3 20개를 등록하고, 빌드 성공 뒤 변경 전 생산 소스에서 실제 기능 Red를 확인한다.
2. SQL은 SET 후위식·힙 스택, 정적 이름/타입 검사와 기존 WHERE를 재사용해 새 행 집합을 준비한다.
3. storage는 임시 완성본을 flush·종료하고 백업을 지정해 실제 `ReplaceFileW`를 호출한다. 실패 상태별 원본 확인·복원·보존을 연결한다.
4. Green·회귀 뒤 고정 소스를 독립 검토하고 새 빌드·실행 QA를 수행한다. 발견한 오류는 실패 재현 뒤 수정한다.
5. Knowledge Manager가 원문·증거를 대조해 문서·Wiki·finish를 마감하고 Master가 전체 결과를 사용자에게 보고한다. 최종 확인 전 DONE으로 바꾸지 않는다.

## 핵심 동작

- UPDATE·DELETE는 WHERE 필수다. UPDATE 대상은 비ID 컬럼이며 대상 중복도 오류다. 행이 없어도 대상·이름·타입 제약을 검사한다.
- SET은 리터럴·NULL·같은 행 컬럼 복사와 INT64 `+`·`-` 연결·괄호를 지원한다. 모든 우변은 동일한 변경 전 행을 본다. NULL 산술은 NULL이고 중간 오버플로도 문장 오류다.
- 부호는 정수 리터럴에만 붙는다. 기존 부호와 숫자의 인접 규칙 및 `--` 주석을 유지한다. 일반 단항 컬럼식·곱셈·나눗셈·함수는 추가하지 않는다.
- UPDATE 건수는 실제 값 변경 여부와 관계없이 WHERE 일치 행 수다. UPDATE·DELETE 0행도 성공이다. 삭제한 ID는 현재 중복이 없으면 명시 INSERT로 재사용할 수 있다.
- 모든 평가·자원·직렬화·성공 결과 준비 전 원본 파일과 live 행을 바꾸지 않는다. 임시 완성본 flush·handle 종료 후 교체 성공이 반영 경계이며 이후 할당 없이 새 행의 소유권을 옮긴다.
- 교체 실패는 반환 코드뿐 아니라 main/temp/backup의 이름·내용을 확인한다. 1177은 main이 없는 상태에서 백업을 덮어쓰기 없이 복원한다. 예상 밖 파일·확인/복원 실패는 보존하고 세션·재시작을 차단한다.
- 반영 후 cleanup·stdout 실패는 전체 성공과 건수를 유지하고 후속 SQL을 멈춘다. 기존 종료 우선순위와 반영/복원 진단을 유지한다.

## Given/When/Then

기본 모델은 `users(id INT64,name TEXT,age INT64,team TEXT)`다. 정렬 없는 결과는 타입·중복을 포함한 행 집합으로 대조한다. 아래 이름은 실제 CTest 등록과 일치하며 여러 변형은 해당 시험 원문을 따른다.

| ID | Given | When | Then |
|---|---|---|---|
| M3-UPDATE-01 | NULL·가변 길이 TEXT와 다른 테이블 | 여러 컬럼 리터럴 수정 | UTF-8·줄바꿈·빈 문자열·NULL 보존, 정확한 건수·재시작·다른 파일 불변 |
| M3-UPDATE-02 | 서로 다른 name/team | `SET name=team,team=name` | old row 기준 교환, 앞 대입이 뒤 우변에 영향 없음 |
| M3-UPDATE-03 | 경계 INT64 | `(age+2)-1`, 연결·괄호·음수와 깊은 식 | 정확한 계산, 기존 INSERT/VALUES 부호·주석 의미 유지 |
| M3-UPDATE-04 | NULL/비NULL 행 | NULL 산술·컬럼 복사·복합 WHERE | NULL 전파, TRUE만 변경, 암묵 타입 변환 없음 |
| M3-UPDATE-05 | 빈 테이블 또는 미일치 | ID/중복 대상·없는 이름·TEXT 산술 | 정적 오류와 원본 바이트 보존 |
| M3-UPDATE-06 | age=0/MAX 두 행 | 후반 행 또는 중간식 overflow | INTEGER_OVERFLOW, 앞 행까지 전부 미반영 |
| M3-UPDATE-07 | 정상 또는 빈 테이블 | 같은 값 대입·미일치 UPDATE | 일치 건수 또는 0행 성공, 세션 계속 가능 |
| M3-UPDATE-08 | 준비된 DB | WHERE 누락·잘못된 식·미지원 연산/AS | 계약 오류·UPDATE 진단·미반영 |
| M3-DELETE-01 | NULL/비NULL 행 | NULL 검사·복합 WHERE·미일치 삭제 | TRUE 행만 삭제, 정확한 건수·재시작 일치 |
| M3-DELETE-02 | 여러 행 | 전체 삭제 뒤 같은 ID INSERT | 유효한 헤더만 남고 명시 ID 재사용·추가 저장 성공 |
| M3-DELETE-03 | 정상/빈 테이블 | WHERE 누락·없는 이름·잘못된 조건 | 정적/문법 오류·DELETE 진단·불변 |
| M3-STORE-01 | 원본과 변경 준비 | 표식·임시 파일·자원 실패, 실제 실행 중 원본 읽기 거부 | 미반영·원본 보존, IO/RESOURCE 구분, execute/data 진단 |
| M3-STORE-02 | 교체 준비 | 1175/1176/기타 오류 상태 주입 | 이름·내용 확인, 원본 보존·정리 뒤 안전한 재시작 |
| M3-STORE-03 | 원본 backup·temp 유지·main 없음 | 1177 복원 | 덮어쓰기 없는 복원·정확한 원본·rollback confirmed |
| M3-STORE-04 | 예상 밖 main 또는 확인/복원 실패 | 오류 처리 | 파일·표식 보존, failed/unknown 구분, 후속 SQL·재시작 차단 |
| M3-STORE-05 | 실제 교체 성공 | 백업/표식 정리 실패 | 성공·건수 유지·exit 5·후속 SQL 중단 |
| M3-STORE-06 | 실제 변경 반영 | stdout 실패·cleanup 동반 | 반영/건수 유지, exit 7 또는 우선하는 5 |
| M3-STORE-07 | 실패 주입 환경 변수 | 운영/시험 실행 파일 비교 | 운영 실행 파일은 주입을 무시하고 정상 실행 |
| M3-CLI-01 | 파일/stdin·table/JSONL | 성공 DML 뒤 오류·추가 DML | 앞선 성공 보존·문장 번호/건수·첫 오류 중단 |
| M3-REGRESSION-01 | M1/M2 데이터·중간 위치 ID·조각 입력 | 수정·삭제·추가·요청/DB 종료 | 포맷·ID·이벤트 소유권 유지, 기존 30개와 신규 20개 회귀 |

## 인수와 보류

독립 QA가 실제 원본 읽기 실패의 진단 문맥 오류 M3-QA-001을 발견했다. 정상 빌드 뒤 해당 실패를 정식 회귀로 먼저 재현하고 최소 수정 후 독립 재검토를 통과했다. M3 누적 품질 반려는 1회이며 해결됐다.

정렬/페이지·JOIN·집계·UNION·migrate와 Phase 2·3는 후속이다. 현재 전체 행 준비·파일 재작성의 메모리/입출력 비용과 기존 시작 시 이차 시간 ID 검사·선형 조회를 유지한다. 전체 204개 인수 명세·대용량 성능·전원 차단 복구·모든 할당/handle 종료 실패를 검증했다고 확대하지 않는다.

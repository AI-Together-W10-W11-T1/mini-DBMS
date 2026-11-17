# Phase 1 스키마 인수 검증 명세

출처: 2026-11-17(화, Asia/Seoul) 현재 스키마 선택과 `Implement the plan` 승인. 원본은 [외부 스키마 계약](../schema-contract.md), SQL 연결은 [SQL 계약](../sql-contract.md)과 [SQL 인수 명세](phase1-sql-acceptance.md)다.

**전체 미실행.** 아래는 Given/When/Then 요구 명세이며 테스트 코드·DBMS 실행 결과가 아니다. 정상 JSON 예시는 계약 §2를 기본값으로 사용하며, 표의 필드 표기는 설명용이다. 실제 fixture는 큰따옴표를 쓰는 JSON으로 작성한다. 각 실패 변형은 별도 입력으로 검증한다. 경로·인자·진단·종료 코드는 [CLI 계약](../cli-contract.md)과 [CLI 인수 명세](phase1-cli-acceptance.md)를 따른다. 저장은 [저장 계약](../storage-contract.md)과 [저장 명세](phase1-storage-acceptance.md)를 따른다.

## P1-SCHEMA-001 — 최소 정상 스키마

- Given: users.json에 계약 §2 예시가 있다
- When: 스키마 디렉터리로 시작한다
- Then: users의 id INT64, name TEXT가 선언 순서로 공개되고 SQL 실행 준비가 된다.
- 상태: 미실행

## P1-SCHEMA-002 — ID만 있는 경계

- Given: users.json의 columns가 [{name:id,type:INT64}]인 유효 JSON이다
- When: 시작한다
- Then: 컬럼 1개 스키마를 허용한다.
- 상태: 미실행

## P1-SCHEMA-003 — 중간 ID·선언 순서

- Given: columns가 name TEXT, ID INT64, age INT64 순서다
- When: 시작 후 SELECT * FROM users;를 실행한다
- Then: ID를 앞으로 이동시키지 않고 name, ID, age 순서로 펼친다.
- 상태: 미실행

## P1-SCHEMA-004 — 혼합 대소문자·원철자

- Given: 파일 Users.JsOn, name Users, 컬럼 Id INT64와 DisplayName TEXT가 있다
- When: 시작 후 users/USERS와 id/ID, displayname/DISPLAYNAME으로 조회한다
- Then: 같은 객체를 찾고 카탈로그 원철자 Users, Id, DisplayName을 보존한다.
- 상태: 미실행

## P1-SCHEMA-005 — 파일 stem 불일치

- Given: users.json의 name이 orders다
- When: 시작한다
- Then: 파일명과 name 불일치로 전체 시작을 실패한다.
- 상태: 미실행

## P1-SCHEMA-006 — 직속 파일만 포함

- Given: 직속 users.JSON은 유효하며 note.txt와 하위 디렉터리 bad.json은 잘못된 내용이다
- When: 시작한다
- Then: users만 로드하고 다른 확장자·하위 디렉터리는 읽지 않는다.
- 상태: 미실행

## P1-SCHEMA-007 — 디렉터리 없음

- Given: 지정한 디렉터리가 없다
- When: 시작한다
- Then: 해당 경로와 원인을 진단하고 SQL 실행 전에 실패한다.
- 상태: 미실행

## P1-SCHEMA-008 — JSON 0개

- Given: 디렉터리가 비었거나 다른 확장자·하위 JSON만 있다
- When: 시작한다
- Then: 대상 JSON 0개로 실패한다.
- 상태: 미실행

## P1-SCHEMA-009 — 열거·읽기 실패

- Given: 디렉터리 열거 또는 직속 대상 JSON 읽기가 실패한다
- When: 시작한다
- Then: 해당 경로/파일과 원인을 진단하고 전체 시작을 실패한다.
- 상태: 미실행

## P1-SCHEMA-010 — 다중 테이블 정상

- Given: users.json과 orders.json 모두 유효하고 이름이 고유하다
- When: 시작한다
- Then: 두 테이블이 모두 검증된 카탈로그로 함께 공개된다.
- 상태: 미실행

## P1-SCHEMA-011 — 뒤늦게 발견한 실패

- Given: 여러 파일 검증 중 앞선 파일은 유효하나 나중에 읽은 대상 파일 하나가 잘못됐다
- When: 시작한다
- Then: 카탈로그를 부분 공개하지 않고 SQL을 한 문장도 실행하지 않는다. 파일 열거 순서는 가정하지 않는다.
- 상태: 미실행

## P1-SCHEMA-012 — 테이블 이름 충돌

- Given: 두 입력 후보의 테이블 이름을 ASCII fold하면 같다
- When: 전체 카탈로그 검증을 한다
- Then: 중복 이름으로 실패한다. Windows에서 두 대소문자 파일 생성이 불가하면 플랫폼 독립 카탈로그 입력으로 검증한다.
- 상태: 미실행

## P1-SCHEMA-013 — 컬럼 이름 충돌

- Given: 같은 테이블에 Name과 name이 있다
- When: 시작한다
- Then: ASCII fold 중복 컬럼으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-014 — 식별자 문법 경계

- Given: 테이블명/컬럼명 후보 _a0, a_9 또는 빈 문자열·9a·a-b·a.b·공백·한글·인용 이름이 있다
- When: 각 후보를 독립 검증한다
- Then: _a0와 a_9만 문법상 허용하고 나머지는 거절한다. 테이블 후보는 파일 stem도 같은 규칙이다.
- 상태: 미실행

## P1-SCHEMA-015 — 예약어 합집합

- Given: 테이블/컬럼 후보가 ABS, select, aSc, NULLS다
- When: 각 후보를 검증한다
- Then: SQL:2023 또는 EBNF 예약어이므로 대소문자와 무관하게 거절한다.
- 상태: 미실행

## P1-SCHEMA-016 — 잘못된 예약어 열 사용 방지

- Given: 테이블/컬럼 후보가 ANALYSE, name, id, text다
- When: 필요한 정상 id를 갖춰 검증한다
- Then: 모두 이름으로 허용한다. PostgreSQL만의 reserved 또는 표준 non-reserved를 잘못 차단하지 않는다.
- 상태: 미실행

## P1-SCHEMA-017 — ID 없음

- Given: 유효한 비ID 컬럼만 있다
- When: 시작한다
- Then: 필수 ID 누락으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-018 — ID 중복

- Given: id와 ID가 함께 선언되어 있다
- When: 시작한다
- Then: 정확히 하나라는 조건/중복 컬럼 위반으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-019 — ID 타입 오류

- Given: id의 type이 TEXT다
- When: 시작한다
- Then: id는 INT64여야 한다는 원인으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-020 — 루트 형태 오류

- Given: JSON 루트가 배열·문자열·정수·불리언·NULL 중 하나다
- When: 각 입력으로 시작한다
- Then: 루트 객체 조건 위반으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-021 — 필수 필드 누락

- Given: 루트의 version/name/columns 또는 컬럼 객체의 name/type 중 하나가 없다
- When: 각 입력으로 시작한다
- Then: 누락 필드 경로와 원인을 진단하고 실패한다.
- 상태: 미실행

## P1-SCHEMA-022 — version 엄격성

- Given: version이 1 이외의 정수, 1.0, 1e0, 문자열 1, true, NULL이다
- When: 각 입력으로 시작한다
- Then: 정수 표기 1만 허용한다는 조건으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-023 — 빈/잘못된 columns

- Given: columns가 [] 또는 배열이 아닌 값이다
- When: 각 입력으로 시작한다
- Then: 비어 있지 않은 배열 조건 위반으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-024 — 컬럼 객체·name 타입

- Given: 컬럼 원소가 객체가 아니거나 루트/컬럼 name이 문자열이 아니다
- When: 각 입력으로 시작한다
- Then: 해당 필드 경로와 구조/타입 원인으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-025 — type 엄격성

- Given: type이 int64, Text, INTEGER, 빈 문자열, 숫자 또는 NULL이다
- When: 각 입력으로 시작한다
- Then: 대문자 INT64/TEXT만 허용한다는 원인으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-026 — 키 대소문자 엄격성

- Given: 필수 키를 VERSION, Name, Columns 또는 Type으로 대체한다
- When: 각 입력으로 시작한다
- Then: 미지 키 또는 필수 필드 누락으로 실패한다. SQL 이름 case folding을 키에 적용하지 않는다.
- 상태: 미실행

## P1-SCHEMA-027 — 미지 필드·제약 거절

- Given: 루트 또는 컬럼에 extra, nullable, primary_key, unique, default를 추가한다
- When: 각 입력으로 시작한다
- Then: 값과 무관하게 미지 필드로 실패하며 엔진 제약을 재정의하지 않는다.
- 상태: 미실행

## P1-SCHEMA-028 — 중복 JSON 키

- Given: 루트 version/name/columns 또는 컬럼 name/type을 동일 객체 안에 두 번 쓴다
- When: 각 입력으로 시작한다
- Then: 첫 값/마지막 값 채택 없이 중복 키로 실패한다.
- 상태: 미실행

## P1-SCHEMA-029 — 해독 후 키 중복

- Given: 동일 객체에 name과 na\u006de 키를 함께 쓴다
- When: 시작한다
- Then: escape 해독 뒤 같은 name 키이므로 중복으로 실패한다.
- 상태: 미실행

## P1-SCHEMA-030 — 정상 JSON escape·키 순서

- Given: 키 순서를 바꾸고 name을 na\u006de, id 값을 \u0069d로 표기한 유효 JSON이 있다
- When: 시작한다
- Then: 해독한 키/식별자로 정상 검증하고 컬럼 배열 순서만 의미 있게 보존한다.
- 상태: 미실행

## P1-SCHEMA-031 — 엄격 JSON 문법

- Given: 대상 파일에 주석, trailing comma, 잘못된 escape, 닫히지 않은 구조 또는 JSON 뒤 비공백이 있다
- When: 각 입력으로 시작한다
- Then: 엄격 JSON 문법 오류로 실패한다.
- 상태: 미실행

## P1-SCHEMA-032 — UTF-8와 시작 BOM

- Given: 같은 정상 JSON을 BOM 없는 UTF-8과 맨 처음 BOM 하나인 UTF-8로 준비한다
- When: 각각 시작한다
- Then: 둘 다 허용한다.
- 상태: 미실행

## P1-SCHEMA-033 — 잘못된 UTF-8

- Given: 대상 JSON 바이트에 잘못된 UTF-8 시퀀스가 있다
- When: 시작한다
- Then: 인코딩 오류로 실패한다.
- 상태: 미실행

## P1-SCHEMA-034 — 진단 경계

- Given: 문법 오류·columns[1].type 오류·읽기 실패·메모리 등 자원 부족을 각각 준비한다
- When: 시작 실패 진단을 받는다
- Then: I/O 실패·자원 부족·문법·스키마 의미 오류를 구분하고 전체 시작 실패·카탈로그 미공개·SQL 미실행을 유지한다. 제공 가능한 파일/경로·원인·JSON 위치/필드 경로를 남긴다. 자원 부족으로 진단 생성이 불가하거나 위치가 불명이면 이를 꾸며 내지 않으며 SQL statement_index/SQL 위치를 조작하지 않는다. wire/종료 코드 자체는 판정하지 않는다.
- 상태: 미실행

## P1-SCHEMA-035 — 실행 중 변경 미반영

- Given: 유효 카탈로그로 시작한 뒤 JSON 컬럼을 추가·변경·삭제하거나 새 파일을 추가한다
- When: 같은 프로세스에서 SQL을 실행한다
- Then: 시작 시 스키마를 계속 사용하며 자동 재로딩하지 않는다.
- 상태: 미실행

## P1-SCHEMA-036 — 재시작 변경 반영

- Given: 실행 종료 후 JSON에 유효한 테이블/컬럼 변경을 남긴다
- When: 재시작하여 카탈로그를 확인한다
- Then: 다시 전체 검증한 새 스키마를 반영한다. 기존 데이터 호환성/마이그레이션 성공은 가정하지 않는다.
- 상태: 미실행

## P1-SCHEMA-037 — 재시작 실패 재검증

- Given: 이전 시작은 성공했으나 이후 JSON을 무효로 바꾸거나 대상 파일을 모두 제거한다
- When: 재시작한다
- Then: 이전 카탈로그로 대체하지 않고 전체 시작을 실패한다.
- 상태: 미실행

## P1-SCHEMA-038 — ID SQL 제약 연결

- Given: 정상 스키마로 시작한 빈 테이블이 있다
- When: id 생략·NULL·0·음수 INSERT, 중복 ID INSERT, SET id=id를 각각 실행한다
- Then: SQL 계약의 ID_REQUIRED/INVALID_ID/DUPLICATE_ID/READ_ONLY_COLUMN 규칙을 유지한다. 삭제 ID의 중복 없는 직접 재사용은 허용한다.
- 상태: 미실행

## P1-SCHEMA-039 — 비ID NULL 연결

- Given: 비ID TEXT/INT64 컬럼을 포함한 정상 스키마가 있다
- When: 필수 id만 INSERT하거나 비ID에 NULL을 명시한다
- Then: 생략 비ID를 NULL로 채우고 명시 NULL도 허용한다.
- 상태: 미실행

## P1-SCHEMA-040 — SQL 문장별 실행 유지

- Given: 유효 스키마와 성공 INSERT→오류 SQL→다음 INSERT 순서의 SQL 파일이 있다
- When: SQL 파일을 실행한다
- Then: 첫 INSERT 성공은 유지하고 SQL 첫 오류에서 중단하며 뒤 INSERT는 미실행이다. 스키마 선검증을 SQL 전체 선검증으로 바꾸지 않는다.
- 상태: 미실행

## P1-SCHEMA-041 — 예약어 고정 집합 회귀

- Given: 계약 부록 A 409개와 EBNF 34개가 있다
- When: 각 단어와 ASCII 대소문자 변형을 이름 후보로 독립 검증한다
- Then: 합집합 모든 단어를 거절한다. 검사 중 외부 사이트 갱신/접속 여부로 집합을 바꾸지 않는다.
- 상태: 미실행

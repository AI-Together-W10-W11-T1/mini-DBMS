# Phase 1 SQL 인수 검증 명세

이 문서는 사용자와 확정한 [SQL 계약](../sql-contract.md)을 구현할 때 사용할 Given/When/Then 명세다. **SQL 엔진과 자동 실행 테스트는 아직 없으며 아래 모든 사례는 미실행이다.** 문서 작성·검토나 운영 하네스 단위 테스트의 통과를 SQL 기능 통과로 표시하지 않는다.

오류 코드의 기호와 정의는 `docs/sql-contract.md`의 고정 오류 코드 표가 단일 원본이다. 아래의 오류 범주는 그 표에 대응한다. 자동 테스트를 연결할 때 해당 코드를 검증하고, 사람이 읽는 오류 설명 전체 문자열에 의존하지 않는다. 데이터·순서·오류 범주와 함께 문장 번호·위치를 검사한다.

명세에서 쓰는 주요 범주와 코드 대응은 다음과 같다. 어휘/문법/비지원은 `LEXICAL_ERROR`/`SYNTAX_ERROR`/`UNSUPPORTED_FEATURE`, 이름은 `UNKNOWN_TABLE`/`UNKNOWN_COLUMN`/`AMBIGUOUS_COLUMN`, 대상/출력 중복은 `DUPLICATE_COLUMN`/`DUPLICATE_OUTPUT_NAME`이다. 타입/개수는 `TYPE_MISMATCH`/`COLUMN_COUNT_MISMATCH`, 리터럴 범위/계산 오버플로는 `INTEGER_RANGE`/`INTEGER_OVERFLOW`, ID는 `ID_REQUIRED`/`INVALID_ID`/`DUPLICATE_ID`/`READ_ONLY_COLUMN`이다. 그룹/정렬/페이지는 `INVALID_GROUPING`/`INVALID_ORDER_BY`/`INVALID_PAGINATION`, 실행 환경 실패는 `IO_ERROR`/`RESOURCE_ERROR`다. 고정 코드의 추가·변경은 계약과 명세를 함께 갱신한다.

## 테스트 준비와 판정 원칙

- 각 사례는 독립적으로 준비하고 실행한다. 앞 사례의 변경 결과에 의존하지 않는다.
- 아래 스키마와 데이터는 설명용 테스트 모델이다. 제공 방식은 [스키마 계약](../schema-contract.md), 검증은 [스키마 인수 명세](phase1-schema-acceptance.md)를 따른다. 저장은 [저장 계약](../storage-contract.md)과 [저장 인수 명세](phase1-storage-acceptance.md)를 따른다. 명령·출력은 [CLI 계약](../cli-contract.md)과 [CLI 인수 명세](phase1-cli-acceptance.md)를 따른다.
- `users(id INT64, name TEXT, age INT64, team TEXT)`, `orders(id INT64, user_id INT64, amount INT64)`를 미리 준비한 것으로 설명한다. 두 테이블의 id는 양수·고유·필수이며 다른 컬럼은 NULL을 허용한다.
- 기본 users 행은 `(1,'Kim',20,'A')`, `(2,'Lee',NULL,'A')`, `(3,'kim',30,NULL)`, `(4,'',-1,'B')`다.
- 기본 orders 행은 `(10,1,100)`, `(11,1,200)`, `(12,2,NULL)`, `(13,NULL,300)`이다. 외래 키 제약을 추가하지 않는다.
- 정렬 없는 결과는 행 순서가 아닌 **타입을 포함한 행 다중집합**으로 비교한다. UNION은 집합, UNION ALL은 중복 개수를 포함해 검사한다. 정렬의 동률을 검증할 때는 고유 id 또는 JOIN 결과에서 고유한 키 조합을 마지막 키로 둔다.
- 오류 사례의 변경 없음은 실행 전후 행 다중집합으로 비교한다. 저장 후 재시작 사례는 별도 프로세스의 SELECT로 확인하도록 자동 테스트 연결 때 구현한다.

## 입력·리터럴·파일 실행

| ID | Given | When | Then |
|---|---|---|---|
| P1-SQL-001 | 빈 users와 준비된 스키마 | `INSERT INTO users(id,name) VALUES(1,'Kim');` 후 재시작하고 `SELECT id,name FROM users WHERE id=1;` | 타입이 INT64/TEXT인 `(1,'Kim')` 한 행. age/team은 NULL이며 저장 값이 유지된다. |
| P1-SQL-002 | 빈 users | `INSERT INTO users(id,name) VALUES(1,'O''Brien;/*x*/--y');` | 따옴표·세미콜론·주석 표시를 포함한 TEXT `O'Brien;/*x*/--y` 한 값으로 저장한다. 추가 문장/주석으로 분리하지 않는다. |
| P1-SQL-003 | 빈 users와 이진 읽기로 준비한 SQL 파일 | 작은따옴표 사이에 실제 CRLF를 넣은 `INSERT INTO users(id,name) VALUES(1,'첫 줄<CR><LF>둘째 줄');` | TEXT에 UTF-8 한글과 CR/LF 바이트를 그대로 보존한다. `<CR><LF>`는 테스트 설명 표기이며 SQL에 그 글자를 입력하는 것이 아니다. |
| P1-SQL-004 | 빈 users | `INSERT INTO users(id,name) VALUES(1,'\n');` | 역슬래시와 n 두 문자로 저장한다. LF로 변환하지 않는다. |
| P1-SQL-005 | 빈 users | `INSERT INTO users(id,age) VALUES(1,-9223372036854775808),(2,9223372036854775807);` | 양끝 정수를 정확하게 저장하고 affected_rows=2다. |
| P1-SQL-006 | 빈 users | `INSERT INTO users(id,age) VALUES(1,9223372036854775808);` | 정수 리터럴 범위 오류. 새 행 0개다. 음수 하한보다 작은 리터럴도 같은 범주로 실패한다. |
| P1-SQL-007 | 준비된 빈 스키마 | 파일 시작 BOM, 탭·공백·줄바꿈, `--` 및 `/* */`, 빈 `;`를 섞고 `iNsErT InTo UsErS(ID,NaMe) VaLuEs(1,'Kim');` | 한 INSERT가 성공한다. 데이터 문자열의 대소문자는 보존한다. 공백·주석·빈 문장만 있는 파일도 no-op 성공이다. |
| P1-SQL-008 | SQL 파일이 정상 INSERT 1개 다음 닫히지 않은 문자열/주석을 포함 | 파일을 실행 | 두 번째 문장에서 어휘 오류로 중단한다. 첫 INSERT는 유지하고 이후 문장은 실행하지 않는다. 오류 위치는 잘못된 문자열/주석을 식별할 수 있어야 한다. |
| P1-SQL-009 | SQL 파일 `INSERT INTO users(id) VALUES(1);` 다음 `INSERT INTO users(id) VALUES(2)`로 EOF 종료 | 실행 | 세미콜론 없는 두 번째 문장은 문법 오류이며 실행하지 않는다. id=1만 남는다. |
| P1-SQL-010 | 정상 INSERT·타입 오류 INSERT·정상 INSERT가 각각 세미콜론으로 끝나는 파일 | 실행 | 첫 성공은 유지, 두 번째는 변경 없이 실패, 세 번째는 미실행이다. 실패 파일 전체를 사전 거절하거나 모두 롤백하지 않는다. |
| P1-SQL-011 | 유효하지 않은 UTF-8 또는 NUL이 현재 SQL 문장에 있음 | 스캔 | 어휘 오류다. 잘못된 입력을 정상 TEXT로 치환하지 않는다. 이전 완료 문장이 있다면 유지한다. |

## INSERT·ID·UPDATE·DELETE

| ID | Given | When | Then |
|---|---|---|---|
| P1-SQL-012 | 빈 users | `INSERT INTO users VALUES(1,'Kim',20,'A');` | 컬럼 목록 누락 문법 오류. 변경 없음. |
| P1-SQL-013 | 빈 users | `INSERT INTO users(id,name) VALUES(1,'Kim'),(2,'Lee');` | 2행 추가, affected_rows=2, 각 age/team은 NULL이다. |
| P1-SQL-014 | 빈 users | `INSERT INTO users(id,name) VALUES(1,'Kim'),(1,'Lee');` | 문장 내부 중복 ID 오류이며 두 행 모두 추가하지 않는다. 기존 ID와 충돌하는 후반 행도 문장 전체 실패다. |
| P1-SQL-015 | 기본 users | id 생략, NULL, 0, 음수, 이미 존재하는 1을 각각 독립 INSERT | 생략은 필수 ID 누락, NULL/비양수는 잘못된 ID, 1은 중복 ID 오류다. TEXT id는 타입 오류이며 각각 변경 없다. |
| P1-SQL-016 | 기본 users | `DELETE FROM users WHERE id=2;` 뒤 `INSERT INTO users(id,name) VALUES(2,'Reused');` | DELETE affected_rows=1, INSERT affected_rows=1. 현재 중복이 없으므로 수동 재사용 성공이다. |
| P1-SQL-017 | 기본 users | `INSERT INTO users(id,name,name) VALUES(5,'A','B');` | 중복 대상 컬럼 오류. 중복 SET 대상도 같은 계약에 따라 오류다. |
| P1-SQL-018 | 기본 users | `UPDATE users SET name=team,team=name WHERE id=1;` | id=1의 name='A', team='Kim'. 대입 순서와 무관하게 두 우변 모두 old row를 본다. affected_rows=1이다. |
| P1-SQL-019 | 기본 users | `UPDATE users SET age=(age+2)-1 WHERE id=1;` | age=21, affected_rows=1. 정수 덧뺄셈 연결·괄호를 처리한다. |
| P1-SQL-020 | 기본 users | `UPDATE users SET age=age+1 WHERE id=2;` | age는 NULL을 유지하고 affected_rows=1이다. |
| P1-SQL-021 | users의 age가 각각 0, INT64_MAX인 2행 | `UPDATE users SET age=age+1 WHERE id>0;` | 한 행의 정수 계산 오버플로로 문장 전체 실패. 첫 행도 0 그대로이고 다른 행도 INT64_MAX 그대로다. |
| P1-SQL-022 | 기본 users | `UPDATE users SET age=(9223372036854775807+1)-1 WHERE id=1;` | 중간 계산 오버플로 오류다. 최종 수학값만 보고 성공시키지 않고 기존 age=20을 유지한다. |
| P1-SQL-023 | 기본 users 또는 빈 users | `UPDATE users SET id=id WHERE id=999;` | 일치 행과 무관하게 ID 수정 금지 오류다. 같은 값을 대입하는 예외도 없다. |
| P1-SQL-024 | 기본 users | `UPDATE users SET name=name WHERE id<=2;` | 저장 값이 같아도 affected_rows=2다. WHERE 미일치는 성공·0행이다. |
| P1-SQL-025 | 기본 users | `UPDATE users SET name='X';` 또는 `DELETE FROM users;` | WHERE 누락 문법 오류이고 전체 데이터 유지다. |
| P1-SQL-026 | 기본 users | `DELETE FROM users WHERE age IS NULL;` | id=2만 삭제하고 affected_rows=1. 미일치 DELETE는 성공·0행이다. |
| P1-SQL-027 | 기본 users | 후반 VALUES 행 컬럼 수 불일치 또는 UPDATE의 TEXT+정수 | 각각 컬럼 수/타입 오류이며 문장 전체 변경 없다. 정상 실패 원자성과 전원 차단·장치 고장 복구 보장은 별개다. |

## WHERE·이름 해석·출력

| ID | Given | When | Then |
|---|---|---|---|
| P1-SQL-028 | 기본 users | `SELECT id FROM users WHERE (age>=20 OR age IS NULL) AND id<4 ORDER BY id;` | `(1),(2),(3)` 순서다. 비교/NULL 검사→AND→OR 우선순위와 괄호를 따른다. |
| P1-SQL-029 | 기본 users | `WHERE age=NULL`, `WHERE age IS NULL`, `WHERE age IS NOT NULL`로 각각 조회 | 첫 결과 0행, 둘째 id=2, 셋째 id=1/3/4다. NULL 비교를 TRUE/FALSE 두 값으로만 처리하지 않는다. |
| P1-SQL-030 | 기본 users | `SELECT id FROM users WHERE 1=id;` 및 `WHERE age=age;` | 첫 결과 id=1, 둘째 id=1/3/4다. 컬럼↔리터럴·컬럼↔컬럼을 허용하며 NULL=NULL은 UNKNOWN이다. |
| P1-SQL-031 | 빈 users | `SELECT missing FROM users;` 및 `SELECT id FROM users WHERE age='20';` | 각각 없는 컬럼/타입 오류다. 행을 만나야만 오류를 내지 않는다. 없는 테이블은 별도의 없는 테이블 오류다. |
| P1-SQL-032 | 기본 users | name='Kim', name='kim', name=''로 각각 조회 | 각각 id=1, id=3, id=4다. 빈 문자열·공백은 다르고 ASCII 대소문자도 데이터에서는 구분한다. |
| P1-SQL-033 | 기본 users | `SELECT name AS id FROM users ORDER BY id;` | 출력 name의 UTF-8 바이트 순서로 정렬한다. 입력 숫자 id로 재해석하지 않는다. `ORDER BY name`은 숨겨진 원래 출력 이름이므로 오류다. |
| P1-SQL-034 | 기본 users | `SELECT name FROM users ORDER BY id;` 및 `SELECT id,id FROM users;` | 각각 비출력 정렬 이름 오류/중복 출력 이름 오류다. 서로 다른 AS를 붙이면 중복 출력 문제를 해결한다. |

## 정렬·페이지·JOIN

| ID | Given | When | Then |
|---|---|---|---|
| P1-SQL-035 | 기본 users | `SELECT id,team FROM users ORDER BY team ASC,id ASC OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY;` | 정렬 후 id 순서가 1/2/4/3이며 반환은 id=2/4다. |
| P1-SQL-036 | 기본 users | `ORDER BY team DESC,id ASC` 및 `ORDER BY team DESC NULLS FIRST,id ASC` | 기본 NULL 마지막에서는 id=4/1/2/3, 명시 FIRST에서는 3/4/1/2다. ASC·DESC에서 NULL 기본 위치를 뒤집지 않는다. |
| P1-SQL-037 | 기본 users | `SELECT id FROM users FETCH FIRST 2 ROWS ONLY;` | 2행 반환을 허용하되 어떤 id인지·순서를 고정하지 않는다. ORDER BY를 임의로 요구하지 않는다. |
| P1-SQL-038 | 기본 users | `SELECT id FROM users OFFSET 0 ROWS;` | 0도 OFFSET 절이므로 ORDER BY 누락 오류다. FETCH→OFFSET 또는 OFFSET→ORDER 같은 잘못된 절 순서는 문법 오류다. |
| P1-SQL-039 | 기본 users | 정렬 뒤 FETCH 0, OFFSET 4, OFFSET INT64_MAX를 각각 적용 | 각각 빈 결과다. OFFSET+FETCH 계산에서 오버플로를 내지 않는다. 음수·INT64 범위 밖 개수는 거절한다. |
| P1-SQL-040 | 기본 users/orders | `SELECT u.id AS user_id,o.id AS order_id FROM users AS u INNER JOIN orders AS o ON u.id=o.user_id ORDER BY user_id,order_id;` | `(1,10),(1,11),(2,12)`다. NULL인 user_id는 연결되지 않는다. |
| P1-SQL-041 | 별도 두 테이블에서 같은 비고유 JOIN 키에 왼쪽 2행·오른쪽 3행 | 키 등호 INNER JOIN | 6개 행 조합. 일치하는 첫 행만 반환하지 않는다. |
| P1-SQL-042 | 기본 users | `SELECT a.id AS a_id,b.id AS b_id FROM users AS a INNER JOIN users AS b ON a.team=b.team WHERE a.id<>b.id ORDER BY a_id,b_id;` | `(1,2),(2,1)`다. self-join과 서로 다른 AS를 지원한다. |
| P1-SQL-043 | 기본 users/orders | `SELECT id FROM users AS u INNER JOIN orders AS o ON u.id=o.user_id;` / `SELECT users.id FROM users AS u;` / `SELECT u.id FROM users AS u INNER JOIN orders AS u ON u.id=u.user_id;` | 순서대로 AMBIGUOUS_COLUMN / UNKNOWN_TABLE / DUPLICATE_TABLE_ALIAS. 어떤 입력도 임의 선택하지 않는다. |
| P1-SQL-044 | 기본 users/orders | `SELECT *` INNER JOIN | 헤더는 `u.id,u.name,u.age,u.team,o.id,o.user_id,o.amount` 순서다. 자동 수식 후 ORDER BY는 표시된 출력 이름만 참조한다. |
| P1-SQL-045 | 기본 users/orders | `SELECT u.id AS uid,o.id AS oid FROM users AS u INNER JOIN orders AS o ON u.id=u.id;` / 같은 출력에서 `ON u.name=o.user_id` / `ON u.id=o.user_id AND o.id=10` | 각각 INVALID_JOIN / TYPE_MISMATCH / UNSUPPORTED_FEATURE. 한쪽만 참조하는 ON과 다중 ON 조건을 허용하지 않는다. |

## 집계·VALUES·UNION

| ID | Given | When | Then |
|---|---|---|---|
| P1-SQL-046 | 기본 users | `SELECT COUNT(*),COUNT(age) FROM users;` | 헤더 count_all/count_age, 한 행 `(4,3)`이다. COUNT 출력에 AS를 강제하지 않는다. |
| P1-SQL-047 | 빈 users | GROUP BY 없는 두 COUNT와 GROUP BY team을 각각 실행 | 전체 집계는 한 행 `(0,0)`, 그룹 쿼리는 0행이다. |
| P1-SQL-048 | 기본 users | `SELECT team,COUNT(*) AS n,COUNT(age) AS known FROM users GROUP BY team HAVING COUNT(*)>=1 AND COUNT(age)>=1;` | A의 `(2,1)`, B의 `(1,1)`, NULL의 `(1,1)` 세 그룹이다. 그룹의 행 순서는 미보장이다. |
| P1-SQL-049 | 팀별 행 수가 9/10/11인 users | `GROUP BY team HAVING COUNT(*)>=10` | 10행·11행 그룹만 반환한다. HAVING의 AND·OR·괄호와 COUNT(컬럼) 비교도 같은 범위로 검증한다. |
| P1-SQL-050 | NULL team을 가진 여러 행 | team으로 GROUP BY | NULL 키는 한 그룹이다. 일반 `NULL=NULL`의 UNKNOWN과 그룹 키 동등 판정을 구분한다. |
| P1-SQL-051 | 기본 users | `SELECT name,COUNT(*) FROM users;` / `SELECT team AS t,COUNT(*) FROM users GROUP BY t;` / `SELECT team FROM users GROUP BY 1;` / `SELECT id FROM users WHERE COUNT(*)>0;` | 순서대로 INVALID_GROUPING / UNKNOWN_COLUMN / UNSUPPORTED_FEATURE / UNSUPPORTED_FEATURE. `COUNT(1)`, `COUNT(DISTINCT age)`, SUM도 UNSUPPORTED_FEATURE다. |
| P1-SQL-052 | 저장 전 행 집합 스냅샷 | `VALUES (1,'Kim'),(2,'Lee');` | column1/column2의 INT64/TEXT 2행을 반환하고 저장·ID 할당은 하지 않는다. |
| P1-SQL-053 | 저장 전 스냅샷 | `VALUES(NULL),(1);` 및 `VALUES(NULL),(NULL);` | 첫 결과 열 타입은 INT64, 둘째는 결과 전용 NULL 상태다. 둘 다 허용하며 NULL을 임의로 TEXT로 고정하지 않는다. |
| P1-SQL-054 | 저장 전 스냅샷 | 후반 VALUES의 컬럼 수 또는 비NULL 타입이 다름 | 각각 컬럼 수/타입 오류다. 앞 결과 행을 부분 성공으로 반환하거나 파일을 변경하지 않는다. |
| P1-SQL-055 | 독립 VALUES 결과 | `VALUES(1,'Kim'),(1,'Kim') UNION VALUES(1,'Kim'),(1,'Lee');` | 전체 행 기준 `(1,'Kim'),(1,'Lee')` 각 1개다. id 한 컬럼만으로 중복 판정하지 않는다. ALL로 바꾸면 총 4행이며 Kim 행은 3개다. |
| P1-SQL-056 | 독립 VALUES 결과 | `VALUES(NULL) UNION VALUES(NULL);` 및 `VALUES(NULL) UNION VALUES(1);` | 첫 결과 NULL 한 행, 둘째 INT64 타입의 NULL/1 각 한 행이다. TEXT 값이 아닌 실제 NULL로 비교한다. |
| P1-SQL-057 | 독립 VALUES 결과 | `VALUES(1),(1) UNION ALL VALUES(2) UNION VALUES(2);` | 좌결합 후 최종 UNION이 누적 결과 전체를 중복 제거하므로 1/2 각 한 행이다. 순서는 미보장이다. |
| P1-SQL-058 | 기본 users | `SELECT id FROM users WHERE id<0 UNION VALUES('1');` | 왼쪽 결과가 0행이어도 INT64/TEXT 타입 불일치 오류다. 컬럼 수가 다른 결합도 거절한다. |
| P1-SQL-059 | 기본 users | `SELECT id AS x FROM users UNION ALL VALUES(9) ORDER BY x;` | 최종 헤더는 첫 분기의 x이며 전체 결과를 정렬한다. 오른쪽 분기 이름으로 ORDER BY하면 오류다. |
| P1-SQL-060 | 서로 다른 분기 결과 | `VALUES(1) ORDER BY column1 UNION VALUES(2);` / `VALUES(1) FETCH FIRST 1 ROWS ONLY UNION VALUES(2);` / `(VALUES(1)) UNION VALUES(2);` | 앞의 두 절 순서는 SYNTAX_ERROR, 괄호 쿼리는 UNSUPPORTED_FEATURE다. 전체 끝의 ORDER/OFFSET/FETCH만 한 번 허용한다. |

## 오류 메타데이터·Phase 호환

| ID | Given | When | Then |
|---|---|---|---|
| P1-SQL-061 | 여러 줄·여러 문장의 SQL 파일에서 두 번째 비어 있지 않은 문장에 오류 | 처리 | 고정 코드, 문장 번호 2, 전체 파일의 행/열, 사람이 읽는 설명을 반환한다. CRLF는 한 줄바꿈으로 계산한다. CLI 표시 형식은 여기서 확정하지 않는다. |
| P1-SQL-062 | 빈 테이블 및 알려진 비지원 문법 | `CREATE TABLE`, MERGE, TRUNCATE TABLE, 독립 TABLE, INTERSECT, EXCEPT, 커서/권한 SQL | 알려진 비지원 기능으로 구분한다. 지원 문법의 잘못된 조합과 없는 테이블·컬럼은 서로 다른 오류 범주다. |
| P1-SQL-063 | I/O·자원 부족을 재현할 테스트 대역 | 해당 실패 지점 실행 | 성공으로 보고하지 않고 I/O·자원 오류를 구분한다. 정상 처리 가능한 실패의 문장 원자성을 검사하되 전원 차단·장치 고장 복구를 이 시험의 보장으로 주장하지 않는다. |
| P1-SQL-064 | 향후 Phase 2 인덱스 연결 전후 동일 데이터 | 같은 `SELECT ... WHERE id=값` 실행 | 논리 결과는 같고 기존 문법은 유지된다. 인덱스 사용 증거와 결과 정확성은 별도로 검사한다. 현재는 Phase 2 실행 미검증이다. |
| P1-SQL-065 | 향후 Phase 2 자동 ID 확장 | 기존 명시 id INSERT 및 id 생략 INSERT | 기존 명시 입력은 계속 유효하고 생략 시 자동 부여한다. 중복·범위·NULL 및 UPDATE id 금지는 유지한다. 생성 알고리즘은 이 명세에서 정하지 않는다. |
| P1-SQL-066 | 향후 Phase 2 파일 재작성/UPDATE/DELETE로 레코드 위치가 바뀜 | 기존 id로 다시 조회 | 파일과 B+ 트리 결과가 일치하고 삭제 행·이전 위치를 잘못 반환하지 않는다. 수정 대상 이외 행의 위치 변경도 검사한다. |
| P1-SQL-067 | 향후 CLI·API가 같은 엔진을 호출 | 동일 성공/오류 SQL 수행 | 타입·NULL·행·affected_rows·오류 의미가 동일하다. 엔진이 stdout에 직접 의존하지 않고 요청별 임시 상태를 분리할 수 있어야 한다. API 프로토콜·Thread Pool·잠금 방식은 정하지 않는다. |

## 구현 연결과 실행 결과 기록

먼저 P1-SQL-001의 입력→파싱→실행→파일 저장/조회 수직 흐름을 구현한다. 이어 복수 행·NULL·조건식·VALUES, UPDATE/DELETE·문장 원자성, 정렬·페이지, JOIN, 집계, UNION 순서로 실제 테스트를 연결한다. 기능 추가 때 관련 오류·경계 사례도 함께 연결하고 누적 회귀 검사를 수행한다.

자동 테스트 연결 시 각 ID에 테스트 이름·실행 명령·실제 결과·근거 위치를 연결한다. 미구현·미실행은 그대로 표시하고 예상 결과만으로 PASS를 기록하지 않는다. 스키마·저장·정상 실패 원자성은 확정 계약을 따르며 CLI 표시는 확정 CLI 계약을 따른다. 명시적 데이터 변환의 타입 변환은 일반 SQL의 암묵적 타입 변환을 허용하는 것이 아니다.

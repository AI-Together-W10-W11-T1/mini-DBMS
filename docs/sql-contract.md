# Phase 1 SQL 문법·동작 계약

출처: 현재 대화에서 사용자가 선택하고 `Implement the plan`으로 승인한 SQL 계약 확정안. 이 문서는 구현할 dialect의 기준이며 SQL 표준 전체를 지원한다는 뜻이 아니다. C23·Windows 구현을 전제로 한다.

## 1. 적용 범위

Phase 1은 INSERT·UPDATE·DELETE·SELECT, 정렬·페이지, 두 테이블 INNER JOIN, COUNT·GROUP BY·HAVING, 독립 VALUES, UNION·UNION ALL을 포함한다. INSERT/SELECT 파일 저장 흐름부터 검증하고 나머지는 의존 순서대로 추가한다.

스키마와 테이블은 사전에 준비되어 있다. 스키마 제공은 [스키마 계약](schema-contract.md)의 테이블별 외부 JSON·시작 전체 검증 방식을 따른다. 저장 형식·문장 반영·실패 처리는 [저장 계약](storage-contract.md), 실행 인자·표시·종료 코드는 [CLI 계약](cli-contract.md)을 따른다. 모듈 수·이름과 Phase 2·3의 상세 구조도 고정하지 않는다.

범위 제외: CREATE TABLE, 권한 관리 SQL, MERGE, TRUNCATE TABLE, 독립 TABLE 조회, INTERSECT, EXCEPT, SQL 커서, 서브쿼리·괄호 쿼리, 세 테이블 이상 JOIN, OUTER JOIN, SELECT DISTINCT, COUNT DISTINCT, COUNT 이외 집계, 파라미터 바인딩. 함수·연산자·자료형은 아래에서 채택한 것만 허용한다.

## 2. 입력과 자료형

| 항목 | 계약 |
|---|---|
| 저장 자료형 | 부호 있는 INT64, UTF-8 TEXT. 각 값은 NULL일 수 있으나 id는 예외다. |
| 정수 리터럴 | 선택적 부호가 있는 십진수. 범위는 -9223372036854775808~9223372036854775807. 실수·지수·16진수 제외. |
| TEXT | 작은따옴표로 감싼다. `''`는 작은따옴표 하나, 역슬래시는 일반 문자다. 빈 문자열·실제 CR/LF·한글·쉼표·세미콜론 허용. |
| 문자열 비교 | UTF-8 바이트 순서, 대소문자·공백 구분. Unicode 정규화·로케일 정렬 없음. WHERE/JOIN/GROUP BY/UNION/ORDER BY에 같은 규칙 적용. |
| 타입 변환 | INT64↔TEXT 암묵적 변환 없음. NULL은 문맥의 타입을 따른다. |
| 이름 | `[A-Za-z_][A-Za-z0-9_]*`. ASCII 대소문자 무시. 선언 이름의 원철자 보존. 예약어·인용 식별자 제외. 예약어는 [스키마 계약](schema-contract.md)의 SQL:2023 예약어와 지원 문법 키워드 합집합을 공유한다. 데이터 문자열의 대소문자는 보존. |
| 파일 | UTF-8, 파일 시작 BOM 허용. 공백·탭·줄바꿈·복수 문장 허용. 문자열의 CR/LF 바이트를 보존. |
| 종료 | 마지막 문장까지 `;` 필수. 빈 문장·공백·주석뿐인 파일은 성공하는 no-op. |
| 주석 | `--` 줄 주석과 비중첩 `/* ... */`. 문자열 안의 주석 표시와 `;`는 데이터. |

닫히지 않은 문자열·주석, 잘못된 UTF-8, 입력 NUL 문자는 어휘 오류다. 문자열·주석 상태를 구분해 문장을 나누며 단순 `split(';')`를 사용하지 않는다.

## 3. 문법

아래 EBNF에서 `[]`는 선택, `{}`는 0회 이상 반복이다. 키워드는 대소문자를 구분하지 않는다. `integer`의 범위는 앞 절을 따르고 `n`은 0 이상의 INT64 정수다. 페이지 수에 계산식·파라미터는 허용하지 않는다.

```ebnf
file          = { ";" | statement, ";" } ;
statement     = insert | update | delete | query ;
insert        = "INSERT INTO", name, "(", name_list, ")", values ;
update        = "UPDATE", name, "SET", assignment,
                { ",", assignment }, "WHERE", predicate ;
delete        = "DELETE FROM", name, "WHERE", predicate ;
assignment    = name, "=", set_expr ;
set_expr      = set_primary, { ("+" | "-"), set_primary } ;
set_primary   = literal | column_ref | "(", set_expr, ")" ;

query         = query_core, { "UNION", ["ALL"], query_core },
                [ "ORDER BY", sort_key, { ",", sort_key } ],
                [ "OFFSET", n, "ROWS" ],
                [ "FETCH FIRST", n, "ROWS ONLY" ] ;
query_core    = select | values ;
select        = "SELECT", ("*" | select_item, { ",", select_item }),
                "FROM", table_ref,
                [ "INNER JOIN", table_ref, "ON", column_ref, "=", column_ref ],
                [ "WHERE", predicate ],
                [ "GROUP BY", column_ref, { ",", column_ref } ],
                [ "HAVING", having_predicate ] ;
table_ref     = name, [ "AS", name ] ;
select_item   = (column_ref | count_expr), [ "AS", name ] ;
count_expr    = "COUNT", "(", ("*" | column_ref), ")" ;
values        = "VALUES", row, { ",", row } ;
row           = "(", literal, { ",", literal }, ")" ;
sort_key      = output_ref, [ "ASC" | "DESC" ],
                [ "NULLS", ("FIRST" | "LAST") ] ;

predicate     = and_term, { "OR", and_term } ;
and_term      = condition, { "AND", condition } ;
condition     = "(", predicate, ")"
              | scalar, comparison, scalar
              | scalar, "IS", [ "NOT" ], "NULL" ;
scalar        = column_ref | literal ;
having_predicate = having_and, { "OR", having_and } ;
having_and    = having_condition, { "AND", having_condition } ;
having_condition = "(", having_predicate, ")"
                 | count_expr, comparison, integer ;
comparison    = "=" | "<>" | "<" | "<=" | ">" | ">=" ;
literal       = integer | string | "NULL" ;
column_ref    = name | name, ".", name ;
output_ref    = name | name, ".", name ;
name_list     = name, { ",", name } ;
```

문법의 `name`은 위 식별자 규칙을 따르며 문자열·정수는 입력 어휘 규칙을 따른다.

추가 의미 검사: OFFSET이 있으면 ORDER BY가 필수다. ON은 양쪽 입력을 각각 참조하는 컬럼 등호 한 개여야 한다. UPDATE SET 대상은 비ID 컬럼이고, SET 덧뺄셈은 INT64 연산이어야 한다. WHERE는 비교·NULL 검사·AND·OR·괄호만 지원하며 일반 NOT·계산식·함수는 제외한다. `*`는 단독 출력 목록이고 `u.*`나 `*, COUNT(*)`는 범위 밖이다.

## 4. 변경·ID·원자성

- INSERT 컬럼 목록은 필수다. 중복 대상 컬럼은 오류다. 복수 행의 값은 리터럴·NULL만 허용한다. 생략한 비ID 컬럼은 NULL이며 DEFAULT 문법은 없다.
- id는 테이블별 양의 INT64·고유·필수다. 생략·NULL·0·음수·중복을 거절한다. 삭제된 ID의 직접 재사용은 현재 중복이 없으면 허용한다.
- UPDATE는 비ID 컬럼 여러 개의 수정·같은 행 컬럼 복사·정수 덧뺄셈 연결·괄호를 지원한다. 모든 우변은 동일한 변경 전 행을 본다. 같은 SET 대상 중복은 오류다.
- SET의 문자열 복사는 가능하지만 곱셈·나눗셈·문자열 연결·함수·서브쿼리는 제외한다. NULL이 포함된 정수 연산은 NULL이다. 중간 계산 오버플로도 문장 오류이며 식을 임의 단순화해 숨기지 않는다.
- `SET id=id`도 수정 금지다. 일치하는 행이 0개여도 대상·이름·타입 제약을 먼저 검사한다.
- UPDATE·DELETE의 WHERE는 필수다. WHERE가 있어도 모든 행에 일치할 수 있으므로 전체 변경을 불가능하게 만드는 보안 장치는 아니다.
- INSERT 결과는 삽입 행 수, DELETE는 삭제 행 수, UPDATE는 WHERE에 일치한 행 수다. 같은 값을 재대입해도 UPDATE 행 수에 포함한다. 일치 행 0개는 성공이다.
- 정상적으로 처리 가능한 오류에서 문장 전체가 성공하거나 실패해야 한다. 다중 INSERT/UPDATE/DELETE의 앞선 행만 반영하지 않는다. 정상 실패 원자성과 파일 전체 트랜잭션·프로세스 강제 종료·전원 차단 복구는 별개이며 후자를 Phase 1에 자동 추가하지 않는다.

## 5. 조회·이름·집계

### 조건·JOIN

조건 우선순위는 괄호 → 비교/NULL 검사 → AND → OR다. 컬럼·리터럴 양쪽 조합과 상수끼리 비교를 허용한다. 정적 타입이 다르면 빈 테이블에서도 TYPE_MISMATCH다.

NULL을 비교 연산에 넣으면 UNKNOWN이다. WHERE/HAVING은 TRUE만 통과한다. IS NULL·IS NOT NULL은 참/거짓을 반환한다. AND/OR는 다음 3값 규칙을 따른다.

| A | B | A AND B | A OR B |
|---|---|---|---|
| TRUE | TRUE | TRUE | TRUE |
| TRUE | FALSE | FALSE | TRUE |
| TRUE | UNKNOWN | UNKNOWN | TRUE |
| FALSE | FALSE | FALSE | FALSE |
| FALSE | UNKNOWN | FALSE | UNKNOWN |
| UNKNOWN | UNKNOWN | UNKNOWN | UNKNOWN |

AND/OR는 대칭이므로 A/B가 바뀐 경우도 동일하다.

INNER JOIN은 한 번만 허용한다. ON은 양쪽 입력의 타입이 맞는 컬럼 하나씩의 등호다. NULL 키는 연결되지 않으며 중복 키는 모든 조합을 반환한다. 같은 테이블을 서로 다른 명시적 AS 별칭으로 self-join할 수 있다. 입력 별칭은 유일해야 하고 별칭을 붙인 테이블은 그 별칭으로만 수식한다. 무수식 컬럼이 두 입력에 존재하면 AMBIGUOUS_COLUMN이다.

### 그룹·COUNT·HAVING

GROUP BY는 하나 이상의 입력 컬럼을 받으며 출력 AS·숫자 위치·계산식은 제외한다. SELECT의 비집계 컬럼은 그룹 키여야 한다. `*`도 펼친 컬럼에 같은 검사를 적용한다. COUNT 인수 컬럼은 그룹 키에 없어도 된다.

COUNT(*)는 모든 행, COUNT(컬럼)는 NULL이 아닌 행을 INT64로 센다. COUNT(1)·COUNT(NULL)·COUNT(DISTINCT ...)는 제외한다. HAVING은 COUNT 식과 정수 리터럴의 6종 비교를 AND·OR·괄호로 결합한다. 일반 컬럼 조건은 WHERE에 쓰고 출력 별칭은 HAVING에서 참조하지 않는다.

GROUP BY 없는 집계는 전체 입력을 한 그룹으로 처리한다. 빈 입력의 COUNT는 0인 한 행이고 GROUP BY가 있는 빈 입력은 0행이다. GROUP BY는 대응 NULL들을 같은 그룹으로 묶는다. 이는 `NULL = NULL`의 UNKNOWN과 용도가 다르다.

### 출력 이름

일반 SELECT 기본 이름은 스키마 선언 원철자의 컬럼명이며 쿼리 참조의 철자로 바꾸지 않는다. JOIN의 직접 컬럼은 `테이블 또는 별칭.컬럼`이다. JOIN의 `*`는 왼쪽 스키마 순서 뒤 오른쪽 스키마 순서다. AS는 기본 이름을 대체하고 원래 이름을 ORDER BY에 남기지 않는다.

COUNT(*)의 기본 이름은 `count_all`, COUNT(컬럼)는 `count_<비수식 컬럼명>`이다. 독립 VALUES는 `column1`, `column2` 순서다. 최종 출력 이름은 대소문자 정규화 후 유일해야 하며 중복이면 DUPLICATE_OUTPUT_NAME이다. 서로 다른 AS로 해결한다.

ORDER BY는 최종 출력 이름만 정확히 참조한다. 입력 테이블로 fallback하거나 숫자 위치로 찾지 않는다. 따라서 `SELECT name AS id ... ORDER BY id`는 출력 name을 정렬하고, `SELECT name ... ORDER BY id`는 오류다. JOIN/UNION에서 `ORDER BY u.id`도 입력 재검색이 아니라 해당 최종 출력 이름을 가리킨다.

## 6. 정렬·페이지·VALUES·UNION

- ORDER BY는 다중 키와 ASC/DESC를 지원하고 생략 방향은 ASC다. ASC/DESC 모두 기본 NULL 마지막이며 키별 NULLS FIRST/LAST로 바꾼다.
- 정렬이 없으면 결과 순서를 보장하지 않는다. 정렬 키가 동률이어도 그 사이 순서를 보장하지 않는다. 페이지 예제에는 고유 id를 마지막 키로 사용하며 JOIN이면 고유 조합을 사용한다.
- FETCH FIRST는 ORDER BY 없이도 허용하고 반환할 행과 순서는 보장하지 않는다. OFFSET은 0이어도 ORDER BY가 필수다. 순서는 ORDER BY → OFFSET → FETCH FIRST다.
- `OFFSET n ROWS`, `FETCH FIRST n ROWS ONLY`를 사용한다. n은 0 이상의 INT64이며 FETCH 0·전체 행 수 이상 OFFSET은 빈 결과다. 두 수를 더할 때 오버플로를 일으키지 않아야 한다.
- 독립 VALUES는 저장·ID 부여 없이 결과 행을 만든다. 모든 행의 컬럼 수와 위치별 비NULL 타입을 검사한다. 오류가 있는 후반 행 때문에 부분 결과를 먼저 노출하지 않는다.
- NULL 타입은 같은 위치의 비NULL 값에서 추론한다. 모든 값이 NULL이면 저장 타입을 추가하지 않고 결과 메타데이터에서만 NULL 전용 상태로 허용한다.
- UNION·UNION ALL은 SELECT/VALUES를 연결한다. 빈 결과인 분기도 컬럼 수·위치별 타입을 검증한다. NULL 전용 열은 상대 INT64/TEXT 타입을 따르며 INT64↔TEXT 변환은 없다.
- UNION은 전체 출력 행의 중복을 제거하고 ALL은 보존한다. 대응 NULL은 같은 중복값이다. id만으로 중복을 제거하지 않는다.
- 여러 결합은 좌결합이다. `A UNION ALL B UNION C`의 마지막 UNION은 앞의 누적 결과 전체도 중복 제거한다. 최종 이름은 첫 분기에서 가져온다.
- 정렬·페이지는 전체 결과 끝에 한 번만 붙인다. 분기별 정렬·괄호 쿼리·서브쿼리는 제외한다.

논리 순서: FROM/JOIN → WHERE → GROUP/COUNT → HAVING → 출력 구성 → UNION → ORDER BY → OFFSET → FETCH.

## 7. 오류와 SQL 파일 실행

문장별로 스캔 → 파싱 → 이름/타입 검증 → 실행한다. 파일 전체를 미리 검증하여 앞선 성공 문장을 실행하지 않는 정책과 섞지 않는다. 첫 오류에서 멈추고 이전 성공은 유지하며 이후 문장은 실행하지 않는다. 마지막 `;`가 없는 문장은 실행하지 않는다.

엔진 오류는 `code`, `statement_index`, `line`, `column`, `message`를 보존하고 CLI가 표시한다. 문장 번호는 비어 있지 않은 SQL 문장 순서, 위치는 파일 전체의 1부터 시작하는 행/열이다. CRLF는 위치 계산에서 한 줄바꿈으로 세되 문자열 바이트는 보존한다. SQL 위치가 없는 파일 I/O 오류 등은 위치 미확인으로 구분한다. CLI 직렬화·출력·종료 코드는 [CLI 계약](cli-contract.md)을 따른다. JSONL의 십진 문자열 표시는 엔진의 INT64 타입을 변경하지 않는다.

| 고정 코드 | 의미 |
|---|---|
| LEXICAL_ERROR | 잘못된 인코딩·문자·닫히지 않은 문자열/주석 |
| SYNTAX_ERROR | 지원 문법의 잘못된 구성·필수 절/세미콜론 누락 |
| UNSUPPORTED_FEATURE | 인식 가능한 범위 밖 명령·기능 |
| UNKNOWN_TABLE | 없는 테이블 |
| DUPLICATE_TABLE_ALIAS | 같은 쿼리의 입력 테이블/별칭 이름 중복 |
| INVALID_JOIN | ON이 서로 다른 양쪽 입력 컬럼을 연결하지 않음 |
| UNKNOWN_COLUMN | 없는 입력 컬럼 |
| AMBIGUOUS_COLUMN | 입력 컬럼의 모호한 참조 |
| DUPLICATE_COLUMN | INSERT/SET 대상 컬럼 중복 |
| DUPLICATE_OUTPUT_NAME | 최종 출력 이름 중복 |
| TYPE_MISMATCH | 타입 불일치·허용되지 않은 타입 연산 |
| COLUMN_COUNT_MISMATCH | VALUES/INSERT/UNION 컬럼 수 불일치 |
| INTEGER_RANGE | 정수 리터럴 범위 초과 |
| INTEGER_OVERFLOW | 계산·COUNT의 INT64 범위 초과 |
| ID_REQUIRED | INSERT의 id 생략 |
| INVALID_ID | NULL·0·음수 id |
| DUPLICATE_ID | 현재 행과 또는 같은 INSERT 안에서 ID 중복 |
| READ_ONLY_COLUMN | id를 SET 대상으로 지정 |
| INVALID_GROUPING | 비그룹 컬럼 출력·잘못된 그룹/집계 사용 |
| INVALID_ORDER_BY | 최종 출력 이름이 아닌 정렬 참조 |
| INVALID_PAGINATION | 음수/범위 초과 페이지 수·OFFSET의 정렬 누락 |
| IO_ERROR | 입출력 실패 |
| RESOURCE_ERROR | 메모리 등 실행 자원 부족 |

모든 임의 오류를 미지원 기능으로 추정하지 않는다. 이름·모호성·타입·컬럼 수·ID 수정 금지는 빈 테이블이나 일치 0행에서도 검증한다. 출력 장치 실패와 저장 변경의 원자성은 구분한다.

저장 반영 확정 후 정리 실패는 [저장 계약](storage-contract.md)에 따라 SQL 성공·affected_rows를 유지하면서 저장 정리 오류를 별도로 보고하고 세션을 중단한다. 이를 미반영된 문장 실패로 표시하거나 이미 확정한 변경을 롤백하지 않는다. 초기 준비·명시적 스키마 변환은 SQL 명령이 아닌 별도 작업이며 일반 SQL의 엄격한 타입 규칙을 변경하지 않는다.

경계 사례의 고정 분류: 별칭을 붙인 뒤 원래 테이블명으로 수식하면 입력 범위에 없는 수식자이므로 UNKNOWN_TABLE, 중복 입력 별칭은 DUPLICATE_TABLE_ALIAS다. ON의 한쪽 입력만 참조하면 INVALID_JOIN, 양쪽 컬럼 타입 불일치는 TYPE_MISMATCH, 다중 ON 조건은 UNSUPPORTED_FEATURE다. GROUP BY가 출력 별칭만 참조해 입력 컬럼을 찾지 못하면 UNKNOWN_COLUMN, 숫자 위치 그룹화와 WHERE의 COUNT·미지원 COUNT 인수/집계는 UNSUPPORTED_FEATURE다. 비그룹 컬럼 출력은 INVALID_GROUPING이다. 분기 ORDER/FETCH 뒤 UNION을 잇는 잘못된 절 순서는 SYNTAX_ERROR, 괄호 쿼리는 UNSUPPORTED_FEATURE다. 정수 리터럴의 부호는 연산 토큰과 구분해 해석하며 `age+1`·`age-1`에 공백을 요구하지 않고 INT64 최솟값도 정확히 파싱한다.

## 8. 대표 SQL

아래 테이블 구성은 검증 명세용 예시다. 제공 방식은 [스키마 계약](schema-contract.md), 바이너리 포맷은 [저장 계약](storage-contract.md)을 따른다. `users(id, name TEXT, age INT64, team TEXT)`, `orders(id, user_id INT64, amount INT64)`에서 두 id는 위 제약을 따르고 나머지는 NULL을 허용한다. 아래는 CREATE TABLE 입력이 아니다.

```sql
INSERT INTO users (id, name, age, team)
VALUES (1, 'O''Brien', 20, 'A'), (2, 'Lee', NULL, 'A');
UPDATE users SET age=(age+1)-0, name=name WHERE id=1;
SELECT id, name FROM users
WHERE (age>=20 OR age IS NULL) AND id>0
ORDER BY name ASC NULLS LAST, id ASC
OFFSET 0 ROWS FETCH FIRST 5 ROWS ONLY;
SELECT id FROM users FETCH FIRST 1 ROWS ONLY;
SELECT u.id AS user_id, o.id AS order_id
FROM users AS u INNER JOIN orders AS o ON u.id=o.user_id
ORDER BY user_id, order_id;
SELECT a.id AS a_id, b.id AS b_id FROM users AS a
INNER JOIN users AS b ON a.team=b.team WHERE a.id<>b.id;
SELECT team, COUNT(*) AS total, COUNT(age) AS known_age FROM users
GROUP BY team HAVING COUNT(*)>=10 AND COUNT(age)>=1
ORDER BY total DESC, team ASC;
VALUES (1, 'Kim'), (2, 'Lee');
VALUES ('첫 줄
둘째 줄');
VALUES (NULL), (1) UNION ALL VALUES (1) UNION VALUES (2)
ORDER BY column1;
DELETE FROM users WHERE id=2;
```

| 오류 SQL | 기대 코드 |
|---|---|
| `INSERT INTO users VALUES (1,'Kim',20,'A');` | SYNTAX_ERROR |
| `UPDATE users SET id=id WHERE id=999;` | READ_ONLY_COLUMN |
| `DELETE FROM users;` | SYNTAX_ERROR |
| `SELECT age FROM users WHERE age='20';` | TYPE_MISMATCH |
| `SELECT name FROM users ORDER BY id;` | INVALID_ORDER_BY |
| `SELECT id FROM users OFFSET 0 ROWS;` | INVALID_PAGINATION |
| `SELECT id FROM users AS u INNER JOIN orders AS o ON u.id=o.user_id;` | AMBIGUOUS_COLUMN |
| `SELECT name,COUNT(*) FROM users;` | INVALID_GROUPING |
| `SELECT COUNT(*) FROM users HAVING SUM(age)>0;` | UNSUPPORTED_FEATURE |
| `VALUES (1,'Kim'),(2);` | COLUMN_COUNT_MISMATCH |
| `VALUES (1) UNION VALUES ('1');` | TYPE_MISMATCH |
| `SELECT id,id FROM users;` | DUPLICATE_OUTPUT_NAME |

## 9. Phase 간 연결과 구현 증분

Phase 2는 기존 SELECT WHERE id=값의 의미를 유지하고 접근 방법에 B+ 트리를 연결한다. 명시 ID INSERT는 유지하고 id 생략 시 자동 부여를 확장한다. UPDATE/DELETE 뒤 저장·인덱스가 일치해야 하며 파일 재작성으로 다른 행의 위치가 바뀌는 경우도 포함한다. 자동 ID 발급·인덱스 갱신·재시작 복원 알고리즘은 Phase 2에서 설계한다.

Phase 3을 위해 엔진은 구조화된 결과/오류를 반환하고 CLI 표시와 분리한다. 파싱·실행 임시 상태는 요청별로 보유한다. API·Thread Pool·잠금·프로토콜 상세는 Phase 3에서 정한다.

Phase 1 내부 구현 순서: 입력·기본 검증·INSERT/SELECT 파일 저장 → 복수 행·NULL·조건식·독립 VALUES → UPDATE/DELETE·계산·문장 원자성 → 정렬·페이지 → JOIN·이름 해석 → COUNT·GROUP BY·HAVING → UNION과 전체 회귀 검사. 각 증분에는 성공·실패·경계값 Given/When/Then 검증을 붙인다. 계약 문서의 존재나 정적 검토를 실제 SQL 실행 통과로 보고하지 않는다.

# Phase 1 외부 스키마 계약

출처: 2026-11-17(화, Asia/Seoul) 현재 대화의 스키마 선택과 `Implement the plan` 승인. 이 문서는 승인한 스키마 제공 계약만 확정한다. SQL 의미는 [SQL 계약](sql-contract.md), 인수 검증은 [스키마 GWT](testing/phase1-schema-acceptance.md)를 따른다. 엔진·자동 테스트·외부 소스 반입·빌드는 아직 수행하지 않았다.

## 1. 입력과 시작

- 외부 JSON 파일 하나가 테이블 하나를 정의한다. 지정한 스키마 디렉터리 직속 파일 중 확장자가 ASCII 대소문자 무시로 `.json`인 파일 전체를 읽는다. 하위 디렉터리와 다른 확장자는 제외한다.
- 디렉터리 없음, 열거/읽기 실패, 대상 JSON 0개는 시작 실패다. 대상 파일 하나라도 문법·의미 오류가 있으면 전체 시작을 실패시키며 SQL을 실행하지 않는다.
- 모든 대상 파일을 검증한 후 카탈로그를 한 번에 공개한다(all-or-nothing). 먼저 읽은 유효 테이블도 부분 공개하지 않는다. 파일 열거 순서나 첫 진단 파일 순서는 보장하지 않는다.
- 시작에 성공한 카탈로그는 프로세스 동안 고정된 스냅샷이다. 실행 중 JSON 추가·변경·삭제를 재로딩하지 않으며 재시작 시 다시 전체 검증한다. 시작 중 외부 파일 변경에 대한 파일시스템 수준 일관성 보장은 별도 설계다.
- 스키마 선검증은 SQL 파일 전체 선검증을 뜻하지 않는다. SQL은 기존대로 문장별 스캔→파싱→검증→실행, 첫 오류 중단, 앞선 성공 유지, 이후 미실행이다.

## 2. JSON 구조

`users.json` 예시:

```json
{
  "version": 1,
  "name": "users",
  "columns": [
    {"name": "id", "type": "INT64"},
    {"name": "name", "type": "TEXT"}
  ]
}
```

| 위치 | 필수 계약 |
|---|---|
| 루트 | 객체. 필드는 정확히 `version`, `name`, `columns` 세 개다. |
| `version` | JSON 정수 `1`만 허용. 문자열·불리언·NULL·실수 표기 `1.0`·지수 표기 `1e0`·다른 버전은 거절한다. |
| `name` | 테이블 이름 문자열. 파일명에서 마지막 JSON 확장자를 뺀 stem과 ASCII 대소문자 무시로 일치해야 한다. |
| `columns` | 비어 있지 않은 배열. 각 원소는 객체이며 배열 순서가 스키마 컬럼 순서다. 객체 키 순서는 의미가 없다. |
| 컬럼 객체 | 정확히 `name`, `type` 두 필수 필드. `name`은 컬럼 이름 문자열이다. |
| `type` | 대문자 문자열 `INT64` 또는 `TEXT`만 허용한다. |

필드 키는 JSON escape를 해독한 후 정확한 소문자 철자를 비교한다. `Name`·`VERSION`은 허용하지 않는다. 루트·컬럼 객체의 미지 필드와 해독 후 중복 키를 모두 거절한다. 예를 들어 `"name"`과 `"na\u006de"`는 중복 키다. JSON 키의 대소문자 엄격성과 SQL 식별자의 대소문자 무시는 서로 다르다.

`nullable`, `primary_key`, `unique`, `default` 등 제약 필드는 허용하지 않는다. 제약은 엔진 규칙으로 적용하며 JSON에서 재정의하지 않는다.

## 3. 이름·중복·ID

- 테이블명·컬럼명·파일 stem은 `[A-Za-z_][A-Za-z0-9_]*`이며 예약어와 인용 식별자를 허용하지 않는다. 한글·공백·점·하이픈·빈 이름은 거절한다.
- 이름의 원철자는 보존한다. 조회·중복 판정·stem 일치·예약어 판정에서만 ASCII A–Z를 a–z로 접는다. Unicode 정규화나 로케일 변환은 하지 않는다.
- 카탈로그 전체 테이블 이름과 각 테이블 내 컬럼 이름은 접은 값으로 고유해야 한다. `Users`/`users`, `Name`/`name` 충돌은 오류다.
- 각 테이블에 접은 이름이 `id`인 컬럼이 정확히 하나 있어야 하며 타입은 `INT64`다. `ID`·`Id`도 같은 ID다. 배열의 어느 위치에 있어도 허용하며 앞으로 이동시키지 않는다.
- SQL의 `SELECT *`와 JOIN의 각 입력 펼침은 선언된 배열 순서를 따른다.
- 기존 SQL 규칙대로 ID는 필수·양의 INT64·테이블별 고유이며 UPDATE 금지다. 비ID 컬럼은 NULL을 허용하고 INSERT에서 생략하면 NULL이다. 스키마에 추가 제약을 쓰지 않는다.

## 4. 예약어의 고정 기준

테이블·컬럼 이름을 검사할 예약어는 아래 부록 A의 **SQL:2023 reserved 집합**과 다음 **현재 SQL EBNF 키워드 34개**의 합집합이다. SQL 이름에도 동일 기준을 적용한다. 예약어 지정은 해당 SQL 기능 구현을 뜻하지 않는다.

```text
ALL AND AS ASC BY COUNT DELETE DESC FETCH FIRST FROM GROUP HAVING INNER
INSERT INTO IS JOIN LAST NOT NULL NULLS OFFSET ON ONLY OR ORDER ROWS
SELECT SET UNION UPDATE VALUES WHERE
```

원본은 [PostgreSQL 18 Table C.1](https://www.postgresql.org/docs/18/sql-keywords-appendix.html)이며 2026-11-17에 확인했다. 추출 규칙: 헤더 `Key Word | PostgreSQL | SQL:2023 | SQL:2016 | SQL-92` 중 **SQL:2023 열**의 공백 제거 값이 정확히 `reserved`인 행의 Key Word를 취한다. `non-reserved`·빈 셀은 제외한다. PostgreSQL 열의 예약/비예약 분류나 SQL:2016·SQL-92 열을 대신 사용하지 않는다. 표시용 U+200B는 제거하고 ASCII 원단어를 보존한다.

부록 A는 이 시점의 로컬 고정 목록이다. 런타임 네트워크 조회나 문서 갱신에 따라 자동 변경하지 않는다. 목록 변경은 계약 변경으로 관리한다. 예: `ABS`는 거절, `ASC`는 EBNF 합집합으로 거절, `ANALYSE`는 PostgreSQL만 예약이므로 이 기준으로 허용, `name`·`id`·`text`는 허용한다.

## 5. 엄격한 파싱과 진단

- UTF-8 JSON을 사용하고 파일 맨 처음 UTF-8 BOM 하나는 허용한다. 잘못된 UTF-8, JSON 주석, trailing comma, 잘못된 escape, 문서 뒤 비공백 등 엄격한 JSON 문법 위반은 거절한다.
- 문법 파싱 성공과 스키마 의미 검증을 구분한다. JSON 라이브러리가 허용하더라도 중복 키·미지 필드·타입·이름·ID 조건을 별도 검사한다.
- 시작 실패 진단에는 실패 파일(디렉터리 자체 문제는 해당 경로)과 원인을 포함한다. 가능한 JSON 위치 또는 필드 경로(예: `columns[1].type`)를 제공하고 위치를 알 수 없으면 미확정으로 구분한다.
- 스키마 오류에 SQL `statement_index`·SQL 행/열을 만들어 넣지 않는다. 진단 형식·인자·종료 코드는 [CLI 계약](cli-contract.md)을 따른다.
- 파일 열거·읽기 등 I/O 실패와 메모리 할당 등 자원 부족을 JSON 문법·스키마 의미 오류와 구분한다. 어느 경우에도 전체 시작 실패·카탈로그 미공개·SQL 미실행을 유지한다. 자원 부족으로 상세 진단을 만들 수 없는 경우 위치를 꾸며 내지 않는다.

## 6. 파서·라이선스·미선택 범위

JSON 파서는 [yyjson 0.13.0 릴리스](https://github.com/ibireme/yyjson/releases/tag/0.13.0)로 고정한다. [같은 태그의 MIT LICENSE](https://github.com/ibireme/yyjson/blob/0.13.0/LICENSE)를 확인했다. 실제 반입 시 해당 태그의 소스·헤더와 MIT 저작권/허가 고지를 함께 보존·동봉한다. 이번 승인은 채택·버전·고지 방침 문서화이며 소스 다운로드/반입·설치·C23 빌드·실행 검증 완료를 의미하지 않는다.

테이블 저장 형식·문장 실패 처리·파일 매핑·스키마 호환성과 명시적 변환은 [저장 계약](storage-contract.md)에 확정했다. 일반 실행은 구조 불일치를 거절하고, 명시적 변환만 원본을 보존하며 새 디렉터리에 전체 결과를 만든다. 인자·표시·종료 코드는 [CLI 계약](cli-contract.md)을 따른다. Phase 2·3은 이 계약을 재사용하되 상세 설계는 해당 Phase에서 정한다.

## 부록 A. SQL:2023 reserved 고정 목록

위 추출 규칙으로 얻은 409개다. `END-EXEC`처럼 식별자 문법 자체로도 배제되는 단어 역시 원본 집합에 그대로 남긴다.

```text
ABS ABSENT ACOS ALL ALLOCATE ALTER AND
ANY ANY_VALUE ARE ARRAY ARRAY_AGG ARRAY_MAX_CARDINALITY AS
ASENSITIVE ASIN ASYMMETRIC AT ATAN ATOMIC AUTHORIZATION
AVG BEGIN BEGIN_FRAME BEGIN_PARTITION BETWEEN BIGINT BINARY
BLOB BOOLEAN BOTH BTRIM BY CALL CALLED
CARDINALITY CASCADED CASE CAST CEIL CEILING CHAR
CHARACTER CHARACTER_LENGTH CHAR_LENGTH CHECK CLASSIFIER CLOB CLOSE
COALESCE COLLATE COLLECT COLUMN COMMIT CONDITION CONNECT
CONSTRAINT CONTAINS CONVERT COPY CORR CORRESPONDING COS
COSH COUNT COVAR_POP COVAR_SAMP CREATE CROSS CUBE
CUME_DIST CURRENT CURRENT_CATALOG CURRENT_DATE CURRENT_DEFAULT_TRANSFORM_GROUP CURRENT_PATH CURRENT_ROLE
CURRENT_ROW CURRENT_SCHEMA CURRENT_TIME CURRENT_TIMESTAMP CURRENT_TRANSFORM_GROUP_FOR_TYPE CURRENT_USER CURSOR
CYCLE DATALINK DATE DAY DEALLOCATE DEC DECFLOAT
DECIMAL DECLARE DEFAULT DEFINE DELETE DENSE_RANK DEREF
DESCRIBE DETERMINISTIC DISCONNECT DISTINCT DLNEWCOPY DLPREVIOUSCOPY DLURLCOMPLETE
DLURLCOMPLETEONLY DLURLCOMPLETEWRITE DLURLPATH DLURLPATHONLY DLURLPATHWRITE DLURLSCHEME DLURLSERVER
DLVALUE DOUBLE DROP DYNAMIC EACH ELEMENT ELSE
EMPTY END END-EXEC END_FRAME END_PARTITION EQUALS ESCAPE
EVERY EXCEPT EXEC EXECUTE EXISTS EXP EXTERNAL
EXTRACT FALSE FETCH FILTER FIRST_VALUE FLOAT FLOOR
FOR FOREIGN FRAME_ROW FREE FROM FULL FUNCTION
FUSION GET GLOBAL GRANT GREATEST GROUP GROUPING
GROUPS HAVING HOLD HOUR IDENTITY IMPORT IN
INDICATOR INITIAL INNER INOUT INSENSITIVE INSERT INT
INTEGER INTERSECT INTERSECTION INTERVAL INTO IS JOIN
JSON JSON_ARRAY JSON_ARRAYAGG JSON_EXISTS JSON_OBJECT JSON_OBJECTAGG JSON_QUERY
JSON_SCALAR JSON_SERIALIZE JSON_TABLE JSON_TABLE_PRIMITIVE JSON_VALUE LAG LANGUAGE
LARGE LAST_VALUE LATERAL LEAD LEADING LEAST LEFT
LIKE LIKE_REGEX LISTAGG LN LOCAL LOCALTIME LOCALTIMESTAMP
LOG LOG10 LOWER LPAD LTRIM MATCH MATCHES
MATCH_NUMBER MATCH_RECOGNIZE MAX MEMBER MERGE METHOD MIN
MINUTE MOD MODIFIES MODULE MONTH MULTISET NATIONAL
NATURAL NCHAR NCLOB NEW NO NONE NORMALIZE
NOT NTH_VALUE NTILE NULL NULLIF NUMERIC OCCURRENCES_REGEX
OCTET_LENGTH OF OFFSET OLD OMIT ON ONE
ONLY OPEN OR ORDER OUT OUTER OVER
OVERLAPS OVERLAY PARAMETER PARTITION PATTERN PER PERCENT
PERCENTILE_CONT PERCENTILE_DISC PERCENT_RANK PERIOD PORTION POSITION POSITION_REGEX
POWER PRECEDES PRECISION PREPARE PRIMARY PROCEDURE PTF
RANGE RANK READS REAL RECURSIVE REF REFERENCES
REFERENCING REGR_AVGX REGR_AVGY REGR_COUNT REGR_INTERCEPT REGR_R2 REGR_SLOPE
REGR_SXX REGR_SXY REGR_SYY RELEASE RESULT RETURN RETURNS
REVOKE RIGHT ROLLBACK ROLLUP ROW ROWS ROW_NUMBER
RPAD RTRIM RUNNING SAVEPOINT SCOPE SCROLL SEARCH
SECOND SEEK SELECT SENSITIVE SESSION_USER SET SHOW
SIMILAR SIN SINH SKIP SMALLINT SOME SPECIFIC
SPECIFICTYPE SQL SQLEXCEPTION SQLSTATE SQLWARNING SQRT START
STATIC STDDEV_POP STDDEV_SAMP SUBMULTISET SUBSET SUBSTRING SUBSTRING_REGEX
SUCCEEDS SUM SYMMETRIC SYSTEM SYSTEM_TIME SYSTEM_USER TABLE
TABLESAMPLE TAN TANH THEN TIME TIMESTAMP TIMEZONE_HOUR
TIMEZONE_MINUTE TO TRAILING TRANSLATE TRANSLATE_REGEX TRANSLATION TREAT
TRIGGER TRIM TRIM_ARRAY TRUE TRUNCATE UESCAPE UNION
UNIQUE UNKNOWN UNNEST UPDATE UPPER USER USING
VALUE VALUES VALUE_OF VARBINARY VARCHAR VARYING VAR_POP
VAR_SAMP VERSIONING WHEN WHENEVER WHERE WIDTH_BUCKET WINDOW
WITH WITHIN WITHOUT XML XMLAGG XMLATTRIBUTES XMLBINARY
XMLCAST XMLCOMMENT XMLCONCAT XMLDOCUMENT XMLELEMENT XMLEXISTS XMLFOREST
XMLITERATE XMLNAMESPACES XMLPARSE XMLPI XMLQUERY XMLSERIALIZE XMLTABLE
XMLTEXT XMLVALIDATE YEAR
```


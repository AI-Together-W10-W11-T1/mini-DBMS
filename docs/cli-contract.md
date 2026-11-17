# Phase 1 CLI 입출력 계약

출처: 2026-11-17 화요일(Asia/Seoul), 현재 대화의 CLI 선택과 문서화 계획에 대한 사용자 `ㄱㄱ` 승인. 이 문서는 구현할 계약이다. **C CLI·SQL 엔진·자동 기능 검사·도구 설치는 아직 수행하지 않았다.** 인수 조건은 [CLI 인수 명세](testing/phase1-cli-acceptance.md)다. SQL 의미·오류 코드는 [SQL 계약](sql-contract.md), 외부 JSON은 [스키마 계약](schema-contract.md), 영속성과 변환은 [저장 계약](storage-contract.md)을 따른다.

## 1. 명령과 인자

아래 `minidb`는 실행 파일의 계약상 이름이다. Windows에서는 `minidb.exe`로 실행한다. S·F·OLD·NEW·NEWSCHEMA·P는 값 자리표시자이며 실제 인자가 아니다.

```text
minidb init --schema-dir S --data-dir NEW [--format table|jsonl]
minidb run --schema-dir S --data-dir D (--file F | --stdin) [--format table|jsonl]
minidb migrate --source-data-dir OLD --schema-dir NEWSCHEMA --data-dir NEW [--plan P] [--format table|jsonl]
minidb --help
minidb --version
minidb init --help
minidb run --help
minidb migrate --help
```

- 하위 명령 하나를 필수로 받는다. 명령·옵션·format 값은 표시한 ASCII 소문자와 정확히 일치해야 한다. 옵션 순서는 자유지만 각 옵션은 한 번만 허용한다.
- 옵션과 값은 별도 argv다. `--key=value`, 짧은 별칭, 위치 인자, 알 수 없는 옵션·명령, 중복 옵션, 값 누락은 ARGUMENT_ERROR·종료 2다. 옵션 토큰을 다음 옵션의 값으로 소비하지 않는다. `--stdin`·`--help`·`--version`은 값을 받지 않는다.
- `run`은 `--file`과 `--stdin` 중 정확히 하나를 받는다. 다른 명령의 옵션이나 `--plan`을 `run`에 붙이면 오류다. 자동 stdin 선택, 기본 스키마/데이터 경로, 환경 변수·설정 파일 fallback은 없다.
- 모든 상대 경로는 프로세스 시작 시 현재 작업 디렉터리 기준이다. 경로의 공백 처리는 호출 셸의 argv 인용 규칙을 따른다. 동일 디렉터리 별칭·겹침 여부와 기존 출력 거절은 저장 계약의 실제 디렉터리 기준을 따른다.
- 기본 format은 `table`이다. 터미널 여부·리디렉션 여부로 자동 변경하지 않는다. REPL·프롬프트·확인 입력은 제공하지 않는다.
- help는 위에 열거한 단독 형태만 허용한다. version도 단독 형태만 허용한다. 이들은 stdout에 UTF-8 도움말 또는 실제 빌드 버전 한 줄을 쓰고 0으로 끝낸다. DB·스키마·잠금·SQL 파일을 열거나 출력 디렉터리를 만들지 않는다. 도움말과 다른 옵션의 혼합은 인자 오류다. 실제 버전 문자열은 빌드 시 정하며 이 문서로 가짜 릴리스 버전을 지정하지 않는다.

## 2. 준비와 SQL 입력

1. 인자와 plan의 JSON 구조를 검증한다.
2. `run`은 SQL 파일의 최초 열기 가능 여부를 확인하고, 각 작업은 스키마·데이터·사용권·미해결 표식·경로·매핑 의미를 검증한다. 준비 단계의 오류로 SQL을 실행하거나 관리 출력 디렉터리를 만들지 않는다. 입력 파일 최초 열기 실패는 IO_ERROR·3이다.
3. `init`은 새 디렉터리 전체를 준비한다. `migrate`는 원본 사용권을 유지하며 새 디렉터리 전체를 복사·변환한다. 성공 공개는 저장 계약의 검증·정리·표식 제거 후다. 하나의 테이블만 완료되어도 전체 성공을 내지 않는다.
4. `run`은 입력 문장을 순서대로 스캔·파싱·검증·실행한다. 파일과 stdin의 정책은 같다. 파일 전체 또는 stdin EOF까지 미리 SQL 검증한 뒤 일괄 실행하지 않는다. 한 문장의 성공 출력 쓰기와 flush를 확인한 뒤 다음 SQL로 진행한다.

SQL 입력은 엄격한 UTF-8이며 시작 BOM 하나를 허용한다. Windows 텍스트 모드 줄바꿈 치환 없이 CR/LF 바이트를 보존한다. CRLF는 SQL 행 위치 계산에서 한 줄바꿈이다. 문자열 안의 실제 CR/LF도 보존한다. 공백·주석·빈 문장은 SQL 계약대로 무시하며 문장 번호를 소비하지 않는다. `statement_index`는 비어 있지 않은 SQL 문장에 대해 1부터 증가한다.

stdin은 세미콜론으로 완성한 문장을 EOF 전에 실행할 수 있다. EOF는 입력 종료이지 파일 전체 트랜잭션 경계가 아니다. 마지막 세미콜론이 없는 SQL은 실행하지 않고 SYNTAX_ERROR·4다. 닫히지 않은 문자열/주석·잘못된 UTF-8은 SQL 계약의 LEXICAL_ERROR·4다. 첫 SQL 오류에서 멈추고 이전 성공·반영을 보존하며 이후 문장을 실행하지 않는다.

최초 열기 후 입력 읽기 도중 장치 오류는 IO_ERROR·4다. 읽기 실패를 정상 EOF로 바꾸지 않으며 불완전 문장을 실행하지 않는다. 이전에 확정한 DML은 유지한다. 데이터 준비 검증이 성공하고 SQL이 공백·주석·빈 문장뿐이면 stdout 없이 0이다. 빈 SQL이어도 잘못된 DB 준비 상태를 건너뛰지 않는다.

## 3. 스트림과 숫자 표현

성공 결과는 stdout, 오류·저장 정리·출력 실패 진단은 stderr다. 진행률·로그·색상·프롬프트를 stdout에 섞지 않는다. 두 스트림 모두 UTF-8 BOM 없이 출력하며 레코드/표시 줄 끝은 LF다. 독립 스트림 사이의 관찰 순서는 보장하지 않으므로 문장 번호와 상태로 대응한다.

JSONL 성공은 물리적 한 줄에 JSON 객체 하나다. 공통 `version`은 JSON number `1`, `type`은 아래의 고정 문자열이다. JSON 객체의 키 순서는 의미가 없고 컬럼·행·테이블 배열 순서는 의미가 있다. 일반 JSON escaping을 적용하며 TEXT의 실제 줄바꿈도 한 줄 안에서 escape한다.

**INT64 데이터 값과 모든 건수·번호·위치·바이트 offset은 JSON 문자열**이다. 부호 없는 건수/위치는 `0` 또는 선행 0 없는 양의 십진수, INT64 값은 불필요한 `+`·선행 0 없는 십진수다. NULL 데이터는 JSON null이고 TEXT는 JSON 문자열이다. 따라서 INT64와 TEXT는 `columns[].type`으로 구별한다. `version`의 `1`만 버전 상수인 number이며 Boolean은 Boolean, 미확인·비해당은 null이다. 문자열화는 JavaScript 정수 정밀도와 무관하게 INT64 전체 범위를 보존한다.

## 4. JSONL 성공 레코드

아래에 적은 필드는 모두 필수이며 알려지지 않은 필드는 포맷 1에서 출력하지 않는다.

| type | 필드와 의미 |
|---|---|
| `query_result` | `version`, `type`, `statement_index`, `columns`, `rows`, `row_count` |
| `statement_ok` | `version`, `type`, `statement_index`, `command`, `affected_rows`, `committed` |
| `operation_ok` | `version`, `type`, `operation`, `output_path`, `tables`, `output_ready` |

- SELECT·독립 VALUES·UNION은 `query_result` 하나를 낸다. `columns`는 최종 출력 순서의 `{"name":이름,"type":"INT64"|"TEXT"|"NULL"}` 배열이다. `NULL` 타입은 SQL 계약의 NULL 전용 결과 컬럼에만 사용한다. `rows`는 컬럼 수와 일치하는 값 배열들의 배열이다. `row_count`는 최종 반환 행 수다. 0행도 `columns`와 `rows:[]`, `row_count:"0"`을 출력한다.
- INSERT·UPDATE·DELETE는 `statement_ok` 하나를 낸다. `command`는 `INSERT`·`UPDATE`·`DELETE`, `affected_rows`는 SQL 계약의 처리 건수, `committed`는 true다. 0행 성공도 같은 형태다. 반영 확정 후 정리 실패도 SQL 성공 상태를 유지한다.
- `init`·`migrate`는 전체 성공 때만 `operation_ok` 하나를 낸다. `operation`은 `init` 또는 `migrate`, `output_path`는 실제 새 출력 디렉터리의 절대 경로, `output_ready`는 true다. `tables`는 ASCII 대소문자 접기 기준 테이블명 오름차순 배열이며 각 항목은 `name`, `action`, `row_count`다. 이름은 새 스키마 원철자다. init은 `created`·`"0"`, migrate는 저장 계약상 동일 파일 `copied` 또는 변환 파일 `converted`와 실제 출력 행 수를 쓴다. 명시 비항등 매핑은 동일 스키마여도 `converted`다.
- 관리 실패에 `operation_ok`나 `output_ready:true`를 내지 않는다. 부분 성공 테이블 레코드도 내지 않는다.
- 쿼리는 전체 결과 계산·타입 검증·직렬화 준비를 완료한 뒤 내보낸다. 후반 VALUES/UNION 오류·자원 부족으로 앞부분만 성공 결과처럼 출력하지 않는다. 출력 장치 자체가 중간 실패하면 잘린 JSON 레코드가 남을 수 있으며 이를 철회할 수는 없다.

```json
{"version":1,"type":"query_result","statement_index":"1","columns":[{"name":"column1","type":"INT64"},{"name":"column2","type":"TEXT"}],"rows":[["9223372036854775807","첫 줄\r\n둘째 줄"],[null,"NULL"]],"row_count":"2"}
{"version":1,"type":"statement_ok","statement_index":"2","command":"UPDATE","affected_rows":"0","committed":true}
{"version":1,"type":"operation_ok","operation":"init","output_path":"C:\\work\\db","tables":[{"name":"users","action":"created","row_count":"0"}],"output_ready":true}
```

## 5. table 표시

table도 잘라내기·폭 제한·생략 없이 모든 결과를 출력한다. 공백 padding이나 테두리를 붙이지 않는 아래 고정 줄 형식을 사용한다. 줄 사이에 빈 줄은 없다.

```text
statement 1 query_result
"column1":INT64|"column2":TEXT
9223372036854775807|"첫 줄\r\n둘째 줄"
NULL|"NULL"
row_count 2
statement 2 UPDATE affected_rows 0 committed true
operation init output_path "C:\\work\\db" output_ready true
table "users" action created row_count 0
```

- 쿼리의 첫 줄은 문장 번호와 `query_result`, 둘째 줄은 최종 이름·타입이다. 각 헤더는 JSON 인용 이름 뒤 `:`와 타입이다. 컬럼 구분자는 `|` 하나다. 0행은 헤더 뒤 바로 `row_count 0`이다.
- 각 데이터 행은 물리적 한 줄이다. NULL은 인용 없는 `NULL`, INT64는 정확한 십진수, TEXT는 빈 문자열도 포함하여 항상 JSON 이중따옴표로 인용한다. JSON escaping에 더하여 모든 인용 문자열의 `|`를 `\u007c`로 표시한다. 제어 문자·따옴표·역슬래시는 JSON escape하며 CR/LF를 실제 표시 줄로 바꾸지 않는다. 헤더·경로·테이블명에도 같은 인용 규칙을 적용한다.
- DML은 예제의 `statement <번호> <command> affected_rows <건수> committed true` 한 줄이다. 관리 성공은 `operation <init|migrate> output_path <인용 경로> output_ready true` 다음에 테이블별 `table <인용 이름> action <action> row_count <건수>`를 JSONL과 같은 순서로 낸다.
- table의 숫자는 표시용 십진 토큰이다. JSONL의 문자열 인코딩 규칙과 의미상 같은 정확한 정수를 나타낸다. 쿼리 전체 준비와 문장 간 flush 확인은 table에도 적용한다.

## 6. 진단 wire와 코드

stderr는 `--format jsonl`이면 진단 하나당 JSONL 객체 하나, 기본 `table`이면 사람이 읽는 한 줄 진단으로 출력한다. 두 형식은 아래의 동일한 논리 정보를 보존한다. table 진단은 `diagnostic code=<코드> operation=<작업> phase=<단계>` 뒤에 아래 표의 나머지 필드를 표 순서로 `키=값` 형태로 표시하며, 문자열은 JSON 인용·escape하고 미확인은 null, 불리언은 true/false로 표시한다. message의 실제 CR/LF는 escape한다. 확인할 수 없는 사실을 만들어 넣지 않는다.

| 필드 | 계약 |
|---|---|
| `version`, `type` | number 1, 문자열 `diagnostic` |
| `code`, `message` | 고정 코드와 사람이 읽는 원인 설명. message 문구 자체는 고정하지 않음 |
| `operation` | `run`·`init`·`migrate` 또는 인자 해석 전 null |
| `phase` | `arguments`·`prepare`·`input`·`execute`·`rollback`·`cleanup`·`output` |
| `source` | `arguments`·`sql_file`·`stdin`·`schema`·`data`·`plan`·`stdout`·`stderr` 또는 미확인 null |
| `path` | 관련 실제 파일/디렉터리 경로 또는 비해당/미확인 null |
| `statement_index`, `line`, `column` | SQL 문장 번호·SQL 전체 입력 기준 1부터 시작하는 위치의 십진 문자열 또는 null |
| `field_path`, `byte_offset` | JSON 오류의 `$` 기반 경로(예: `$.tables[0].map[1].source`)·0부터 시작하는 원본 바이트 위치 문자열 또는 null |
| `committed` | SQL DML 반영 확정 true, 미반영 확인 false, 비해당/미확인 null |
| `rollback` | `not_needed`·`confirmed`·`failed`·`unknown` |
| `command`, `affected_rows` | 해당 DML의 대문자 command와 확정 처리 건수 문자열. 비해당/미확인은 null |
| `output_ready` | 관리 출력 준비 완료 true, 미완료 false, 비해당/미확인 null |

`line`·`column`은 SQL 위치 전용이다. JSON 오류를 SQL 위치처럼 표시하지 않으며 field_path·byte_offset 중 알아낸 정보만 낸다. SQL 입력의 BOM은 첫 SQL 문자의 열을 늘리지 않는다. 원본 byte_offset에는 BOM을 포함한다. 읽기 I/O 오류는 문장 번호를 알면 보존하되 모르는 line/column은 null이다. 정상 실패는 원본을 건드리지 않았으면 rollback not_needed, 복원 확인이면 confirmed다. 복원 실패는 failed, 상태 불명이면 unknown이며 committed를 단정하지 않는다.

SQL 계약의 고정 코드 23개는 그대로 보존한다: LEXICAL_ERROR, SYNTAX_ERROR, UNSUPPORTED_FEATURE, UNKNOWN_TABLE, DUPLICATE_TABLE_ALIAS, INVALID_JOIN, UNKNOWN_COLUMN, AMBIGUOUS_COLUMN, DUPLICATE_COLUMN, DUPLICATE_OUTPUT_NAME, TYPE_MISMATCH, COLUMN_COUNT_MISMATCH, INTEGER_RANGE, INTEGER_OVERFLOW, ID_REQUIRED, INVALID_ID, DUPLICATE_ID, READ_ONLY_COLUMN, INVALID_GROUPING, INVALID_ORDER_BY, INVALID_PAGINATION, IO_ERROR, RESOURCE_ERROR. SQL 오류를 관리 코드로 재명명하지 않는다.

| 추가 관리 코드 | 용도 |
|---|---|
| ARGUMENT_ERROR | CLI 인자·plan JSON 구문/구조/중복 키 오류 |
| SCHEMA_ERROR | 외부 스키마 JSON·선언 검증 실패 |
| STORAGE_FORMAT_ERROR | 저장 헤더·레코드·행/ID 제약 등 저장 형식 검증 실패 |
| SCHEMA_MISMATCH | 일반 실행의 외부 스키마와 저장 스키마 비호환 |
| DATABASE_BUSY | 동일 실제 디렉터리 사용권 획득 실패 |
| RECOVERY_REQUIRED | 시작 때 발견한 미해결 표식·작업 잔여물, 실행 중 복원 실패·상태 불명으로 차단 필요 |
| MIGRATION_ERROR | 테이블 집합·매핑/삭제 의미 오류 또는 명시적 변환 값 검증 실패 |

파일/장치 접근 실패는 IO_ERROR, 자원 부족은 RESOURCE_ERROR를 보존한다. 같은 원인에 상충하는 코드 여러 개를 임의로 붙이지 않는다. 코드만으로 종료값을 추론하지 말고 단계·저장 상태도 함께 본다.

## 7. 종료값과 실패 이후

| 값 | 의미 |
|---|---|
| 0 | 전체 성공, 빈 SQL 성공, help/version 성공 |
| 2 | 인자 오류 또는 plan JSON의 구문·구조 오류 |
| 3 | 출력 디렉터리 생성/SQL 실행 전 준비 실패: 입력 최초 열기, 경로·스키마·데이터·잠금·미해결 표식·매핑 의미 검증 |
| 4 | SQL 실행/중간 입력 읽기 실패, 생성 시작 후 init/migrate 실패. DML 실패는 미반영 또는 복원 확인 상태 |
| 5 | DML 반영 확정 뒤 저장 정리 실패. 성공·처리 건수는 유지하고 다음 SQL 중단 |
| 6 | 실행 중 롤백 실패·미확인 또는 예상 밖 저장 상태로 원본 건강/반영 여부가 미해결인 중단 |
| 7 | stdout write/flush 실패. 다음 SQL 중단 |

복수 오류가 발생하면 **6 > 5 > 7 > 4 > 3 > 2** 우선순위로 종료값 하나를 선택하고 각 진단은 보존한다. 정상적인 불완전 init/migrate 출력은 `output_ready:false`·4이며 원본 건강이 미해결인 6과 혼동하지 않는다. 시작 시 이미 있는 표식의 차단은 3이다.

stdout 실패는 이미 반영한 DML의 롤백 이유가 아니다. 다음 SQL은 실행하지 않는다. stdout 성공 레코드가 잘리거나 전혀 전달되지 않아도 stderr에 code IO_ERROR·phase output과 `committed:true`, `command`, 확정 `affected_rows`를 남긴다. 저장 정리도 실패하면 정리 진단도 보존하고 5, 원본 건강이 미해결이면 6을 우선한다. 관리 작업 준비가 이미 끝난 후 stdout만 실패하면 `output_ready:true`·7이다.

반영 확정 뒤 저장 정리 실패는 성공 레코드(출력 가능할 때)와 별도 cleanup 진단을 남기며 자동 재실행·롤백하지 않는다. 임시/백업·표식을 보존하는 저장 계약을 따른다. stderr 자체의 write/flush 실패는 진단 전달을 보장할 수 없는 한계이며 기존 원인의 종료값을 유지한다. stderr 실패를 이유로 DB 작업을 재실행하거나 성공으로 바꾸지 않는다.

## 8. migrate plan JSON

`--plan`은 선택이다. 없으면 모든 테이블에 기본 동일 이름 대응과 빈 drop을 적용한다. 아래는 JSON 구조 예시이며 실제 이름 존재 여부는 원본·새 스키마를 대조해 검증한다.

```json
{
  "version": 1,
  "tables": [
    {
      "name": "users",
      "map": [{"target": "full_name", "source": "name"}],
      "drop": ["legacy"]
    }
  ]
}
```

- 루트는 object이며 `version`과 `tables`를 모두 필수로 받는다. version은 JSON number의 정수 1, tables는 배열이다. 다른 루트 필드는 금지한다. `tables:[]`는 허용한다.
- tables 원소는 object이며 문자열 `name`이 필수다. `map`·`drop`은 선택이며 생략하면 각각 빈 배열이다. map 원소는 `target`, `source` 두 필수 문자열만 갖는 object, drop 원소는 문자열이다. 나머지 필드·잘못된 타입·null 대체는 거절한다.
- UTF-8 BOM 하나는 허용한다. 엄격한 UTF-8 JSON이며 주석·후행 쉼표·잘못된 Unicode·디코딩 후 같은 object 키 중복·알 수 없는 필드를 거절한다. 이들은 ARGUMENT_ERROR·2다. JSON 키 이름은 정확한 대소문자를 사용한다.
- plan 파일 접근 실패는 IO_ERROR·3이다. 구문/구조 검증 후 테이블·컬럼 이름은 스키마 식별 규칙인 ASCII 대소문자 접기로 비교한다. 빈/잘못된 식별자, 테이블 중복, 대상 중복, 원본 중복 재사용, drop 중복·참조 오류는 의미 검증이다.
- plan에 생략한 테이블은 동일 이름 기본 대응·drop 빈 배열이다. 원본·새 테이블 집합 일치, 명시 대응 우선, 미대응 새 비ID 컬럼 NULL, 미사용 원본 전부 명시 drop, id 유지 등 **매핑 의미와 변환 규칙 전체는 저장 계약 7절**을 따른다. 사용하지 않는 매핑도 거절한다.
- 의미상 잘못된 매핑·삭제·테이블 집합은 출력 생성 전 MIGRATION_ERROR·3이다. 실제 행의 TEXT→INT64 파싱·범위 오류는 변환 실행 중 MIGRATION_ERROR·4이며 원본을 보존하고 새 출력은 미완료로 차단한다. I/O·자원 오류는 각각 기존 코드를 쓴다.

## 9. 검증과 후속 Phase

문서·예제·링크·인수 ID의 정적 검토와 실제 CLI 프로세스 실행을 구분한다. 본 계약의 명령은 현재 실행 가능하다고 주장하지 않는다. [인수 명세](testing/phase1-cli-acceptance.md)의 모든 사례는 미실행이며, 이후 승인된 C 구현에서 실제 프로세스 stdout/stderr·종료값·저장 상태·실패 주입으로 검증한다. Phase 2/3는 이 SQL·결과 의미를 유지하며 접근 방법/API를 확장하고, 이 문서로 API 프로토콜·Thread Pool 상세를 확정하지 않는다.

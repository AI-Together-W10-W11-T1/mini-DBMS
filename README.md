# mini-DBMS

Windows 네이티브 C23로 만드는 소형 DBMS다. 현재 작업 상태와 다음 행동은 [HANDOFF](HANDOFF.md)를 따른다.

## 빌드와 검사

저장소 루트의 PowerShell에서 실행한다. Python은 운영·검사 도구용이며 DB 엔진 구현은 C다. 고정 도구는 프로젝트 `.cache/tooling`에 설치돼 있다. 새 환경에서는 [도구 lock](tools/c-toolchain-lock.json)과 설치 스크립트를 먼저 확인한다.

```powershell
python scripts/c_checks.py build
python scripts/c_checks.py regression
```

각 명령은 설치된 고정 Clang/CMake/Ninja/cmocka 경로를 명시해 실제 configure·compile·link를 수행한다. `regression`은 이어서 CTest198개(M1~M8 123개·M9 12개·M10 15개·M11 8개·M12 18개·M13 14개·M14 8개)를 실행한다. `MINIDB_CC`, `MINIDB_CMAKE`, `MINIDB_NINJA`, `MINIDB_CTEST`, `CMOCKA_SOURCE_DIR` override를 설정하지 않았을 때 lock에 기록된 기본 경로를 사용한다. 셸의 전역 PATH는 바꾸지 않는다.

기본 검사 부산물은 `.cache/bootstrap/scaffold/manual/manual/<build|regression>/`에 생긴다. 실행 파일은 다음과 같다.

```powershell
$exe = '.\.cache\bootstrap\scaffold\manual\manual\regression\build\minidb.exe'
& $exe --version
& $exe --help
```

`CMakePresets.json`은 Ninja 구성을 제공하지만 preset만으로 고정 컴파일러·도구 경로를 선택하지 않는다. 위 검사 명령이 고정 경로를 연결한다. `minidb_fault.exe`는 실패 주입 전용 시험 실행 파일이며 일반 실행에는 `minidb.exe`를 쓴다.

## 첫 실행

예를 들어 새 `demo-schema/users.json`을 UTF-8로 준비한다.

```json
{"version":1,"name":"users","columns":[{"name":"id","type":"INT64"},{"name":"name","type":"TEXT"},{"name":"age","type":"INT64"}]}
```

`insert.sql`과 `query.sql`도 UTF-8 파일로 준비한다. 각 SQL은 세미콜론으로 끝낸다.

```sql
-- insert.sql
INSERT INTO users(id,name,age) VALUES(1,'Kim',25);
```

```sql
-- query.sql
SELECT id,name,age FROM users WHERE id=1;
```

기존 디렉터리를 덮어쓰지 않으므로 `demo-data`는 존재하지 않는 새 경로여야 한다.

```powershell
& $exe init --schema-dir .\demo-schema --data-dir .\demo-data --format jsonl
& $exe run --schema-dir .\demo-schema --data-dir .\demo-data --file .\insert.sql --format jsonl
& $exe run --schema-dir .\demo-schema --data-dir .\demo-data --file .\query.sql
```

마지막 명령은 별도 프로세스에서 저장 파일을 읽고 다음 표를 출력한다.

```text
statement 1 query_result
"id":INT64|"name":TEXT|"age":INT64
1|"Kim"|25
row_count 1
```

`--file` 대신 `--stdin`도 지원한다. stdin은 엄격한 UTF-8 바이트를 보내야 하며 PowerShell 버전에 따라 파이프 문자 인코딩이 다르다. 기본 출력은 table, `--format jsonl`은 자동 검증용 JSONL이다. 성공은 stdout, 진단은 stderr이며 경로는 항상 명시한다.

## M2 SQL 예제

위 첫 실행 뒤 다음을 UTF-8 `m2.sql`로 저장해 같은 run의 `--file` 인자로 실행할 수 있다.

```sql
INSERT INTO users(id,name,age) VALUES(2,'Lee',NULL),(3,'Park',30);
SELECT u.id AS user_id,u.name AS user_name
FROM users AS u WHERE (u.age>=20 OR u.age IS NULL) AND u.id<>1;
VALUES (NULL),(1);
VALUES (NULL),(NULL);
```

복수 INSERT의 후반 행에서 타입·ID 오류가 나면 해당 문장 전체를 반영하지 않는다. VALUES는 DB 파일을 변경하지 않고 결과를 만든다. 별칭을 붙인 입력은 그 별칭으로 수식하며 출력 AS는 WHERE의 입력 컬럼 이름을 바꾸지 않는다.

## M3 SQL 예제

위 M2 예제 뒤 다음을 UTF-8 파일로 저장해 같은 DB에 실행할 수 있다.

```sql
UPDATE users SET age=(age+2)-1,name=name WHERE id<=2;
DELETE FROM users WHERE age IS NULL;
INSERT INTO users(id,name,age) VALUES(2,'Reused',40);
SELECT id,name,age FROM users;
```

모든 SET 우변은 변경 전 행을 본다. ID는 수정할 수 없고 UPDATE·DELETE는 WHERE가 필수다. 후반 행이나 중간 계산에서 오류가 나면 문장 전체를 반영하지 않는다. 임시 완성본 교체가 성공한 뒤 정리·출력이 실패한 경우에는 성공과 건수를 유지한다. [M3 결과](docs/testing/m3-results.md)에 실패·복원·재시작 검증과 한계를 기록했다.

## M4 SQL 예제

다음은 위 스키마와 기존 DB에서 실행할 수 있는 정렬·페이지 예제다.

```sql
SELECT id,name,age FROM users
ORDER BY age DESC NULLS LAST,id ASC
OFFSET 0 ROWS FETCH FIRST 2 ROWS ONLY;
SELECT name AS chosen FROM users ORDER BY chosen;
VALUES (NULL,3),(2,1),(1,2)
ORDER BY column1 DESC NULLS FIRST FETCH FIRST 2 ROWS ONLY;
```

정렬은 최종 출력 이름만 참조한다. OFFSET은 0이어도 ORDER BY가 필요하며 FETCH FIRST는 단독 사용 가능하다. ASC/DESC 모두 기본 NULL 마지막이다. 빈 결과나 FETCH 0도 정적 오류와 후반 VALUES 오류를 숨기지 않는다. [M4 결과](docs/testing/m4-results.md)에 실제 검사와 한계를 기록했다.

## M5 JOIN 예제

위 users 스키마와 기존 DB에서 다음을 UTF-8 파일로 저장해 `run --file`로 실행할 수 있다. 서로 다른 명시적 AS를 쓰는 self-join 예제다.

```sql
SELECT a.id AS left_id,b.id AS right_id,a.name AS left_name
FROM users AS a INNER JOIN users AS b ON a.id=b.id
ORDER BY left_id,right_id FETCH FIRST 2 ROWS ONLY;
```

ON은 서로 다른 입력의 같은 타입 컬럼 등호 하나다. NULL 키는 연결되지 않고 중복 키의 모든 조합을 보존한다. 기본 출력 이름은 `a.id`처럼 입력 이름을 포함하며 ORDER BY는 최종 출력 이름만 참조한다. [M5 결과](docs/testing/m5-results.md)에 실제 검사와 한계를 기록했다.

## M6 집계 예제

위 users 스키마와 기존 DB에서 다음을 UTF-8 파일로 저장해 `run --file`로 실행할 수 있다.

```sql
SELECT COUNT(*),COUNT(age) FROM users;
SELECT age,COUNT(*) AS total FROM users
GROUP BY age HAVING COUNT(*)>=1
ORDER BY total DESC,age;
```

COUNT(column)은 NULL을 제외한다. NULL 키는 한 그룹으로 묶으며 COUNT 없는 GROUP BY와 HAVING 전용 COUNT도 지원한다. 전체 빈 집계는 COUNT0 한 행, 빈 그룹 집계는0행이다. 비집계 출력은 그룹 키여야 한다. [M6 결과](docs/testing/m6-results.md)에 실제 검사와 한계를 기록했다.

## M7 UNION 예제

위 DB에서 다음을 UTF-8 파일로 저장해 `run --file`로 실행할 수 있다.

```sql
SELECT id AS chosen FROM users UNION ALL VALUES(9)
UNION VALUES(9) ORDER BY chosen;
```

혼합 결합은 좌결합이며 UNION은 누적 전체 행을 중복 제거한다. 첫 분기의 출력 이름을 사용하고 정렬·페이지는 전체 결과 끝에 한 번 적용한다. [M7 결과](docs/testing/m7-results.md)에 실제 검사와 한계를 기록했다.

## M8 migrate 예제

첫 실행의 users(id,name,age) DB를 사용한다. `new-schema/users.json`과 `migration-plan.json`을 UTF-8로 준비한다. 아래는 시험한 rename·drop·재배열·신규 NULL 대응을 기존 예제 스키마에 적용한다.

```json
{"version":1,"name":"users","columns":[{"name":"renamed","type":"TEXT"},{"name":"id","type":"INT64"},{"name":"fresh","type":"TEXT"}]}
```

```json
{"version":1,"tables":[{"name":"users","map":[{"target":"renamed","source":"name"}],"drop":["age"]}]}
```

`migrated-data`는 새 경로여야 한다. 성공 뒤 새 스키마·새 데이터 경로를 지정한다.

```powershell
& $exe migrate --source-data-dir .\demo-data --schema-dir .\new-schema --data-dir .\migrated-data --plan .\migration-plan.json --format jsonl
& $exe run --schema-dir .\new-schema --data-dir .\migrated-data --file .\migrated-query.sql --format jsonl
```

`migrated-query.sql`에는 `SELECT * FROM users ORDER BY id;`를 저장한다. 기존 id와 name 값이 id와 renamed에 대응하고 fresh는 NULL이다. 원본 DB는 보존된다. plan 생략 시 동일 이름 대응·빈 drop을 사용한다. 실패·출력 준비 상태·실제 검증 한계는 [M8 결과](docs/testing/m8-results.md)를 따른다.

## M10 자동 ID 예제

기존 users 스키마에서 `INSERT INTO users(name) VALUES('Auto');`를 SQL 파일에 저장해 `run --file`로 실행하면 현재 최대 ID+1을 부여한다. 빈 테이블은1이며 최대 ID 삭제 뒤 해당 값이 재사용될 수 있다. [M10 결과](docs/testing/m10-results.md)에 인덱스 연계·실제 검사·한계를 기록했다.

## M11 대량 생성·비교

위 regression 빌드 뒤 새 캐시 목적지를 지정한다. `--rows`를10000·100000·1000000으로 바꾸고 각각 다른 새 목적지를 사용한다. 실제 CLI INSERT·전 필드 바이너리 검증·한 DB open 뒤 공개 API QPC 조회 비교를 실행하며 원시값·통계는 목적지 report.json에 저장한다.

```powershell
python scripts/m11_benchmark.py --exe .cache/bootstrap/scaffold/manual/manual/regression/build/minidb.exe --bench .cache/bootstrap/scaffold/manual/manual/regression/build/bench_m11.exe --destination .cache/m11-example/rows-10000 --rows 10000
```

현재 측정은 Debug·메모리 적재 뒤 warm SQL 조회이며100만 행 실제 실행은1회다. 단계별 시간·독립 검증·시작 비용의 한계는 [M11 결과](docs/testing/m11-results.md)를 따른다.

## M12 서버·정적 SDK·원격 CLI

첫 실행에서 만든 `demo-schema`·`demo-data`와 `query.sql`을 사용한다. 서버가 DB 디렉터리를 소유하므로 로컬 CLI 작업을 마친 뒤 서버를 실행한다.

```powershell
$build = '.\.cache\bootstrap\scaffold\manual\manual\regression\build'
& "$build\minidbd.exe" --schema-dir .\demo-schema --data-dir .\demo-data --port 15432
```

별도 PowerShell에서 저장소 루트로 이동해 SDK CLI를 실행한다. 서버는 Ctrl+C로 종료한다.

```powershell
& '.\.cache\bootstrap\scaffold\manual\manual\regression\build\minidb-remote.exe' --host 127.0.0.1 --port 15432 --file .\query.sql --format jsonl
```

`--stdin`은 EOF 뒤 전체 SQL을 제출한다. 기본 bind는127.0.0.1이며 자원·시간 옵션과 반영/미확인 상태의 의미는 [M12 결과](docs/testing/m12-results.md)를 따른다.

같은 CMake 프로젝트의 SDK 사용 대상은 `include/minidb_client.h`를 포함하고 다음 타깃에 연결한다. CMake가 SDK의 yyjson·Winsock 의존성을 연결한다.

```cmake
target_link_libraries(my_client PRIVATE minidb_client)
```

`mdc_options_default` 후 host·port를 지정하고 `mdc_connect`→`mdc_execute`→`mdc_next`로 호출한다. EVENT마다 `md_event_destroy`를 호출하며 DONE 뒤 연결을 재사용하고 마지막에 `mdc_close`한다. 실제 함수·오류 필드는 [공개 SDK 헤더](include/minidb_client.h)를 따른다.

## 공유 읽기와 SQL 실행 제어

서버는 병렬 읽기·배타 쓰기를 지원한다. `minidbd.exe`에 `--sql-timeout-ms 1000`을 추가하면 최종 문장 gate 취득 뒤 SQL 실행에 협력 기한을 적용한다. 기본0은 비활성이며 양수 상한은 INT64_MAX다. 저장 보호구간의 반영·복원은 완료한다.

로컬 C 호출자는 `MdExecutionOptions`를 초기화해 `md_request_next_ex(request, &options, &event)`로 lock/SQL 기한과 취소 callback을 전달한다. 기존 `md_request_next`도 유지한다. callback/context 수명·요청 병렬 호출 조건은 [공개 헤더](include/minidb.h), 취소·반영·SDK 호환 한계는 [M13 결과](docs/testing/m13-results.md)를 따른다.

## M14 서버 성능 측정

최종 검증한 Debug 빌드 경로를 사용한다. 목적지는 프로젝트 `.cache` 아래의 새 경로여야 하며 기존 파일을 덮어쓰지 않는다. 기본값은10,000행·연결1/4/16/32·ID/scan_key/혼합95/5·warmup20회 뒤40회·3반복이다.

```powershell
$build = '.cache/agent-harness/m14-native-20261119/artifacts/VERIFY/b/817458ad21'
python scripts/m14_benchmark.py --exe "$build/minidb.exe" --server "$build/minidbd.exe" --bench "$build/bench_m14.exe" --destination .cache/m14-example/full-1
```

짧은 실제 smoke는 같은 명령의 새 목적지에 `--rows 80 --connections 1 --repeats 1 --requests 20 --workloads id`를 추가한다. 스크립트가 fixture 생성·실제 SDK 부하·서버 종료·원시 보고서/큐 기록·전체 최종 데이터 검증을 수행한다. 목적지 `report.json`의 `valid/complete`와 `all_requests_succeeded`는 다르며 큐 포화 오류도 기록한다. 서버 단독 계측은 `--queue-metrics <새 파일 경로>`로 활성화하며 기본은 비활성이다. SQL/EOF 큐 대기는 DB gate 대기가 아니다.

원래 측정의 무효1회·재측정·실제 검사와 짧은 Debug/loopback 조건의 한계는 [단일 M14 결과](docs/testing/m14-results.md), 동작/GWT는 [계약](docs/implementation-m14.md)을 따른다.

## 현재 범위와 한계

- `INSERT`는 컬럼 목록이 필수이고 한 행 또는 복수 행을 입력한다. 양의 고유 INT64 `id`는 명시하거나 생략해 자동 발급하며 생략한 비ID 컬럼은 NULL이다.
- `SELECT *` 또는 컬럼 목록, 한 테이블 FROM 또는 두 입력 INNER JOIN·입력 AS·수식 참조·출력 AS를 지원한다. WHERE는 6종 비교·IS NULL/IS NOT NULL·AND/OR·괄호와 NULL 3값 논리를 적용한다. 단일 테이블 ID 등호와 AND 필수 등호는 인덱스로 후보를 찾고 전체 조건을 재평가한다. OR와 일반 비ID 조건은 선형 검색한다.
- 엄격한 스키마·저장 전체 검증, 실제 프로세스 사용권, 미해결 표식 차단, append 실패 복원과 반영 후 정리·출력 실패 상태를 검증했다. 실제 stdin EOF 전 실행과 별도 프로세스 재시작도 검사했다.
- UPDATE는 비ID 다중 대입·컬럼 복사·INT64 덧뺄셈/괄호, DELETE는 기존 WHERE를 지원한다. 임시 완성본 교체·실패 상태 확인/복원·재시작을 검증했다. SELECT·VALUES·JOIN·집계의 UNION/ALL과 전역 다중 정렬·페이지를 지원한다. `migrate`는 헤더 기반 원본 검증·명시 컬럼 대응/drop·정확한 INT64↔TEXT 변환·새 디렉터리 전체 공개와 관련 관리 실패 처리를 지원한다.
- 시작 시 전체 행을 검증·메모리에 적재하고 테이블별 ID 인덱스를 구성한다. JOIN은 중첩 루프 O(L×R), 선형 그룹 탐색은 최악 이차 시간이며 그룹·결과 전체를 메모리에 보관한다. M11에서100만 행 적재·전 필드 정확성·ID/선형 조회를 검증했다. 메모리 효율·시작 비용 원인·전원 차단/강제 종료 복구와 모든 손상·할당 실패는 미검증이다.

현재 단계와 다음 행동은 [HANDOFF](HANDOFF.md)를 따른다.

## 문서와 운영

- [요구사항](docs/requirements.md), [SQL](docs/sql-contract.md), [스키마](docs/schema-contract.md), [저장](docs/storage-contract.md), [CLI](docs/cli-contract.md) 계약
- [M5 구현·실행 증거](docs/testing/m5-results.md), [M4 구현·핵심 불변식](docs/implementation-m4.md), [M4 실행 증거](docs/testing/m4-results.md), [M3 이력](docs/testing/m3-results.md), [M2 이력](docs/testing/m2-results.md), [M1 이력](docs/testing/m1-results.md), [작업 기록](docs/work-log.md), [남은 작업](docs/pending-decisions.md)
- [설정 상태](docs/setup-status.md), [제3자 라이선스](docs/third-party-licenses.md), [하네스](docs/workflow/agent-harness.md), [AGENTS.md](AGENTS.md)

```powershell
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/dev_tools.py status
python scripts/dev_tools.py finish
```

이 Python 단위 검사는 하네스·운영 도구용이며 위 CTest DBMS 기능 검사와 구분한다. 원본 `codex-setup-github/`는 보존하며 현재 프로젝트에 일괄 restore하지 않는다.

## 발표 — 파일에서 작은 RDBMS로 

**README 1분 → CLI 시연 45~60초.** 

### 1. 문제 정의 

> “파일은 데이터를 담습니다. 저는 그 안의 **행을 식별하고, 조건으로 찾고, 함께 써도 일관성을 지키는 작은 RDBMS**를 만들었습니다.”

**파일 경로 → 파일 찾기 / SQL 조건 → 행 찾기**

Codd의 관계형 모델은 **물리적 배치에서 질의를 분리**하고 중복·일관성 문제를 다뤘다. [1970년 원문](https://research.ibm.com/publications/a-relational-model-of-data-for-large-shared-data-banks)

### 2. 문제 해결 

```text
INSERT / SELECT → 문법·타입 검사 → 메모리 행 + 바이너리 파일
                                      └ 자동 ID → B+ 트리 → 행 번호
로컬 CLI → 같은 엔진
원격 CLI / C SDK → TCP → 네트워크 루프 → 워커 풀 → 같은 엔진
```

| 설계 | 선택과 대가 |
|---|---|
| 저장·실패 | INSERT는 추가·실패 복원 / UPDATE·DELETE는 임시 파일 교체 |
| 식별·검색 | 주소 대신 논리 ID / 메모리 B+ 트리 → 빠른 ID 조회, 시작 시 재구성 |
| 동시성·기한 | 병렬 읽기·배타 쓰기 / 대기 writer 우선 진입 정책, 양수 SQL 기한은 협력 취소 |
| 구현 도구 | C23·Clang / CMake·Ninja / LLDB / cmocka·CTest / yyjson·Winsock |

### 3. 일반 파일 작업과 비교 지표 

| 비교 | 관측 결과 |
|---|---:|
| **100만 행 ID 조회 vs 선형 조회** | **37.2µs vs 15.67ms → 약 421배** |
| 서버 ID 조회 · 16접속 | **11,027 req/s · 성공 p95 1.776ms** |
| 서버 SQL 큐 대기 · 16접속 | p95 0.415ms · 오류 0/1,920 |

첫 행은 **같은 엔진의 Debug·warm 조회 p50**, 서버 수치는 **짧은 Debug·loopback 3반복 통계의 중앙값**이다. PowerShell 대비 배율이나 운영 용량 보장은 아니다. [조회 근거](docs/testing/m11-results.md) · [서버 근거](docs/testing/m14-results.md)

**시연 전 준비** — 저장소 루트의 PowerShell에서 한 번 실행한다. 빌드는 앞의 안내를 사용한다.

```powershell
.\demo.ps1 setup
```

기본 실행 파일은 현재 저장소의 M14 검증 빌드다. 다른 경로에 빌드했다면 ` .\demo.ps1 setup -Exe '빌드경로\minidb.exe'`로 지정한다. 새 setup은 이전 시연 데이터를 지우지 않고 별도 실행을 준비한다.

**시연** — 두 명령이 각각 SQL과 PowerShell 작업을 한 쌍으로 실행하고 결과·실제 시간을 표시한다.

```powershell
.\demo.ps1 insert   # INSERT ↔ Add-Content: 같은 행 추가
.\demo.ps1 select   # WHERE id=... ↔ Select-String: 같은 행 찾기
```

시연은 작은 표본으로 진행한다. DB 시간에는 **새 CLI 시작·전체 행 검증·인덱스 재구성**이 포함되어 PowerShell보다 느릴 수 있다. 파일 추가는 DB의 검증·복원·flush와 작업량도 다르다. 발표에서는 **행 의미와 일치 결과**, 알고리즘 비교에서는 위의 **동일 엔진 측정**을 설명한다. 실제 리허설 시간과 조건은 [시연 결과](docs/testing/presentation-results.md)를 따른다.

<details>
<summary>1분 발화 예시 — 표 전체를 읽지 않고 핵심만 설명</summary>

저는 파일에 데이터를 넣는 데서 그치지 않고, 행의 의미와 규칙을 관리하는 작은 관계형 DBMS를 만들었습니다.
파일 이름을 찾는 운영체제의 인덱스와 달리, 이 DBMS는 SQL 조건으로 행을 찾고 타입과 고유 ID를 검사합니다.
SQL 처리기와 바이너리 저장 구조를 C로 만들었고, 쓰기 실패 복원과 임시 파일 교체를 구현했으며 전원 손실 자동 복구는 범위에서 제외했습니다.
주소는 재시작이나 삭제로 달라지므로 논리 ID를 부여하고, 메모리 B+ 트리가 현재 행 번호로 연결하며 시작할 때 인덱스를 다시 만듭니다.
같은 엔진을 로컬 CLI와 원격 C SDK에서 재사용하고, 워커 풀의 읽기는 병렬로 처리하되 쓰기는 배타 실행하여 처리량과 일관성을 함께 고려했습니다.
100만 행의 warm 조회에서는 ID 검색이 선형 검색보다 약 421배 빨랐고, 이제 작은 표본으로 SQL과 PowerShell의 추가·검색 결과와 실제 시간을 비교하겠습니다.

발화 속도에 따라 달라지므로 1분은 리허설 목표이며, 세부 표와 아래 답변은 읽지 않는다.

</details>

<details>
<summary>질문 대비 — OS와 DBMS, 저장 구조, ID와 인덱스</summary>

| 질문 | 설명 |
|---|---|
| OS도 파일을 검색하는데? | NTFS의 디렉터리 B-tree나 ext4의 hash tree는 **파일명→파일 위치**를 찾는다. 이번 DB 인덱스는 **테이블의 ID→메모리 행 번호**를 찾는다. 파일 내용의 `age=25` 조건과 타입·고유성은 별도 계층이다. [NTFS](https://learn.microsoft.com/en-us/windows/win32/fileio/file-streams) · [ext4](https://cdn.kernel.org/doc/html/latest/filesystems/ext4/directory.html) |
| grep·PowerShell과 SQL의 차이? | `grep '^10001,' users.csv`에 해당하는 PowerShell은 `Select-String -Path users.csv -Pattern '^10001,'`이다. 줄 패턴 검색은 구조화된 타입·NULL·ID 중복 검사까지 제공하지 않는다. OS는 파일 잠금·저널링 같은 기반 기능을 제공하고 DBMS는 그 위에 행의 규칙을 구현한다. [Select-String](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.utility/select-string?view=powershell-5.1) |
| 행은 어떻게 저장하나? | 테이블별 가변 길이 바이너리, 스키마 헤더·NULL 구분·길이 정보. INSERT는 append 후 오류 시 원래 길이로 복원하고, UPDATE·DELETE는 완성한 임시 파일로 교체한다. 단순 `Add-Content`는 문자열을 덧붙인다. [저장 계약](docs/storage-contract.md) · [Add-Content](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/add-content?view=powershell-5.1) |
| 주소가 있는데 왜 ID가 필요한가? | 메모리 주소·행 번호는 재시작·삭제 압축으로 바뀐다. ID는 행의 논리적 정체성이고 인덱스가 현재 위치로 연결한다. 자동 발급은 편의이며 핵심은 고유성이다. 현재 최댓값+1 방식은 최댓값 삭제 뒤 그 번호를 다시 쓸 수 있어 영구 비재사용 ID는 아니다. |
| 해시 대신 B+ 트리를 쓰는 이유? | 해시는 등호 검색에 적합하고, B+ 트리는 정렬 순서·범위·순차 순회를 함께 지원하는 자료 구조다. **현재 SQL의 인덱스 적용은 ID 등호와 AND 필수 등호이며, 범위 조건은 스캔**한다. 일반적인 B-tree 장점과 구현한 최적화를 구분한다. [인덱스 종류](https://www.postgresql.org/docs/current/indexes-types.html) |
| 장애 복구까지 완성했나? | 문장별 원자성·복원 실패 차단·확정 반영 보존을 구현했다. WAL 기반 재시작 복구·전원 손실 복구·전체 SQL 표준·완전한 ACID 트랜잭션을 구현했다고 주장하지 않는다. |

</details>

<details>
<summary>질문 대비 — 도구의 역할, CLI·SDK 사용 모양, 동시성 trade-off</summary>

| 필요한 도구 | 이유 |
|---|---|
| Windows 네이티브 C23 + LLVM/Clang | 엔진·메모리·파일 수명을 직접 구현하고 컴파일·링크 |
| CMake + Ninja | 의존 대상·고정 도구를 구성하고 필요한 대상을 빌드 |
| LLDB / cmocka + CTest | 상태·호출 흐름 디버깅 / 단위·통합 검사의 실행과 집계 |
| yyjson / Winsock | 외부 스키마·응답 JSON의 엄격한 처리 / TCP 서버와 SDK 통신 |

**CLI 모양** — 서버 실행 뒤 다른 PowerShell에서 SDK 기반 원격 CLI를 사용한다. `query.sql`에는 `SELECT id,name,age FROM users WHERE id=1;`처럼 SQL을 쓴다.

```powershell
& "$build/minidbd.exe" --schema-dir .\demo-schema --data-dir .\demo-data --port 15432
& "$build/minidb-remote.exe" --host 127.0.0.1 --port 15432 --file .\query.sql --format jsonl
```

`$build`는 앞의 검증된 실행 파일 디렉터리이며, 스키마·데이터와 SQL 파일은 첫 실행 안내로 준비한다. 위 `demo.ps1`은 자체 시연 데이터를 따로 만든다.

**C SDK 모양** — 헤더 `minidb_client.h`, 정적 라이브러리 `minidb_client`. 아래는 핵심 호출 흐름이며 오류 처리까지 포함한 완전한 예제 프로그램은 아니다.

```c
MdClientOptions options;
mdc_options_default(&options);       /* host·port 등 지정 */
mdc_connect(&options, &client, &error);
mdc_execute(client, (const unsigned char *)sql, strlen(sql), &error);
MdClientNext next = mdc_next(client, &event, &error);
if (next == MDC_EVENT) md_event_destroy(&event); /* DONE까지 반복 */
mdc_close(client);
```

실제 사용자는 각 반환값을 확인하고 `mdc_next`의 EVENT·DONE·ERROR를 구분한다. [공개 SDK](include/minidb_client.h) · [실제 SDK 호출 코드](src/remote_cli.c)

**병렬 처리 선택** — 네트워크 루프는 소켓, 4개 워커는 SQL을 맡는다. 유휴 연결마다 워커를 점유하지 않는 대신 완료 전달·소유 수명을 관리한다. 대기 작업 기본16개가 차면 `SERVER_BUSY`를 반환한다.

읽기는 공유하고 쓰기는 배타 실행한다. writer 대기 중 새 reader를 막아 쓰기 지연을 줄이는 대신 읽기 지연이 늘 수 있으며 엄격 FIFO·기아 방지는 보장하지 않는다. SQL 기한 기본0은 비활성이고 양수는 안전 지점에서 확인한다. 저장 보호구간은 반영 또는 필요한 복원·정리를 끝내므로 즉시 강제 중단과 다르다. [M13 결과](docs/testing/m13-results.md)

**서버 수치의 경계** — 32접속 ID 부하의 `SERVER_BUSY`는 1,752/3,840건이었다. 별도로, 전체 선택36개 실행에서 실행별 관측 최대 private bytes는 6.55~10.47MiB였다. 메모리는 표본 최대이고, 큐 대기는 DB 잠금 대기와 다르다. 기본값 조정·자동 튜닝은 수행하지 않았다. 최종 제품 회귀 **198/198 통과**와 별개로 짧은 시연은 운영 성능 보장이 아니다. [M14 결과](docs/testing/m14-results.md)

주제별 학습 질문은 [학습 가이드](docs/study-guide.md)를 따른다.

</details>

# mini-DBMS

Windows 네이티브 C23로 만드는 소형 DBMS다. 첫 실행 마일스톤(M1)은 `init`·단일 행 `INSERT`·기본 `SELECT`·바이너리 저장·프로세스 재시작 조회까지 구현했고 독립 QA를 통과했다. **최종 사용자 확인 대기**이며 전체 Phase 1 완료는 아니다. 실제 결과는 [M1 검증 기록](docs/testing/m1-results.md), 재개 순서는 [HANDOFF.md](HANDOFF.md)를 따른다.

## 빌드와 검사

저장소 루트의 PowerShell에서 실행한다. Python은 운영·검사 도구용이며 DB 엔진 구현은 C다. 고정 도구는 프로젝트 `.cache/tooling`에 설치돼 있다. 새 환경에서는 [도구 lock](tools/c-toolchain-lock.json)과 설치 스크립트를 먼저 확인한다.

```powershell
python scripts/c_checks.py build
python scripts/c_checks.py regression
```

각 명령은 설치된 고정 Clang/CMake/Ninja/cmocka 경로를 명시해 실제 configure·compile·link를 수행한다. `regression`은 이어서 CTest 16개를 실행한다. `MINIDB_CC`, `MINIDB_CMAKE`, `MINIDB_NINJA`, `MINIDB_CTEST`, `CMOCKA_SOURCE_DIR` override를 설정하지 않았을 때 lock에 기록된 기본 경로를 사용한다. 셸의 전역 PATH는 바꾸지 않는다.

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

## 현재 범위와 한계

- `INSERT`는 컬럼 목록이 필수이고 한 행을 입력한다. 양의 고유 INT64 `id`는 직접 제공하며 생략한 비ID 컬럼은 NULL이다.
- `SELECT *` 또는 컬럼 목록, 한 테이블 `FROM`, 선택적 단일 `WHERE 컬럼 = 리터럴`을 지원한다. ID와 비ID 조건 모두 현재 선형 검색이다.
- 엄격한 스키마·저장 전체 검증, 실제 프로세스 사용권, 미해결 표식 차단, append 실패 복원과 반영 후 정리·출력 실패 상태를 검증했다. 실제 stdin EOF 전 실행과 별도 프로세스 재시작도 검사했다.
- 나머지 SQL과 `migrate`는 후속 Phase 1 증분이다. help에 migrate 계약이 표시돼도 변환 기능은 아직 구현하지 않았다.
- 시작 시 행을 메모리에 적재하고 ID 중복 검사가 이차 시간이다. 대용량 성능·메모리 효율·100만 건 결과를 주장하지 않는다. 전원 차단/강제 종료 복구와 모든 손상 탐지는 보장하지 않는다.

Phase 2는 기존 엔진에 메모리 B+ 트리·자동 ID·100만 건 비교를, Phase 3은 API·Thread Pool을 연결한다. 후속 상세 설계는 해당 Phase에서 사용자와 정한다.

## 문서와 운영

- [요구사항](docs/requirements.md), [SQL](docs/sql-contract.md), [스키마](docs/schema-contract.md), [저장](docs/storage-contract.md), [CLI](docs/cli-contract.md) 계약
- [M1 구현·핵심 불변식](docs/implementation-m1.md), [실행 증거](docs/testing/m1-results.md), [작업 기록](docs/work-log.md), [남은 작업](docs/pending-decisions.md)
- [설정 상태](docs/setup-status.md), [제3자 라이선스](docs/third-party-licenses.md), [하네스](docs/workflow/agent-harness.md), [AGENTS.md](AGENTS.md)

```powershell
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/dev_tools.py status
python scripts/dev_tools.py finish
```

이 Python 단위 검사는 하네스·운영 도구용이며 위 CTest DBMS 기능 검사와 구분한다. 원본 `codex-setup-github/`는 보존하며 현재 프로젝트에 일괄 restore하지 않는다.

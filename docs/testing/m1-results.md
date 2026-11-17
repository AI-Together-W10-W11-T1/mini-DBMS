# M1 실제 구현·검증 결과

2026-11-17 화요일(Asia/Seoul). 승인된 첫 실행 마일스톤 `DBMS-001`, 정식 run `m1-native-20261117`의 증거다. **구현·독립 QA 통과, 최종 사용자 확인 대기**다. 전체 Phase 1 또는 204개 인수 명세 완료를 의미하지 않는다.

## 최종 상태와 증거

최종 기술 소스 digest는 `6480aeac9ecce378a1a7def6ac46753dbaaec01b8f6ab7a83bd623dac74256a8`다. 아래 영수증은 `.cache/agent-harness/m1-native-20261117/commands/` 기준이며 각 디렉터리의 `receipt.json`, 로그, 기능 검사에는 `test-report.json`이 있다. 캐시는 Git 추적·배포 아티팩트가 아니므로 삭제되면 고정 소스로 재실행해야 한다.

| 단계 | 실제 결과 | 증거 디렉터리 |
|---|---|---|
| 최초 scaffold build | configure·compile·link 성공 | `1-build` |
| 최초 기능 Red | CTest exit 8, 단위 1 PASS·CLI 15 런타임 FAIL, report errors 빈 배열 | `2-red` |
| 첫 구현 Green/회귀 | 16/16 PASS, 당시 소스에서 실행 | `10-green`, `11-regression` |
| 첫 독립 검토 | 기본 회귀 16 PASS·추가 36 PASS이나 실제 help/version stdout 실패 진단 누락으로 REWORK | `12-build`, `13-regression`, reviewer_qa-1 결과 |
| 좁은 수정 Red | 새 실제 read-only stdout 회귀가 변경 전 CLI에서 실패, M1-argv-wire FAIL·나머지 15 PASS | `14-build`, `15-red` |
| 최종 구현 build/Green/회귀 | 최종 digest에서 각각 exit 0, CTest 16/16 PASS | `16-build`, `17-green`, `18-regression` |
| 최종 독립 QA | 별도 VERIFY build·회귀 16/16 PASS, report errors 빈 배열 | `19-build`, `20-regression` |

첫 Red의 실제 실패는 `init`을 실행한 뒤 `UNSUPPORTED_FEATURE`/exit 3을 받아 성공 기대 assertion이 깨진 것이다. 도구·import·컴파일 실패를 기능 Red로 바꾸지 않았다. 두 번째 Red는 실제 stdout handle에 쓸 수 없는 상태를 만들어 초기 help 경로가 성공으로 끝나는 결함을 재현했다. 실패 주입 환경변수로 이 결함을 재현한 것이 아니다.

최종 Reviewer 결과는 `.cache/m1-master/reviewer-result-2.json`과 정식 `results/reviewer_qa-2.json`에 review APPROVED, qa PASS로 분리돼 있다. Reviewer는 실제 소스를 고정해 검토했고 자신의 단계에서 새 configure/compile/CTest를 수행했다.

## 등록된 기능 시험 16개

| 시험 | 검증한 동작 | 최종 독립 결과 |
|---|---|---|
| M1-unit-event-lifetime | 0 초기화/반복 해제, 요청·DB 종료 뒤 이벤트 소유권, 결정적인 1바이트 callback의 BOM/UTF-8/CRLF/escape, read-ahead·중간 input 오류 | PASS; 내부 cmocka 함수 3개 |
| M1-roundtrip | 파일 INSERT, 별도 프로세스 SELECT, 타입·생략 NULL, 기본 표 정확한 바이트 | PASS |
| M1-literals | BOM/주석, 작은따옴표 escape, 한글·CRLF·역슬래시 보존, INT64 최솟값 | PASS |
| M1-stop-on-error | 타입 오류에서 중단, 앞선 반영 유지·뒤 문장 미실행, 마지막 세미콜론 필수 | PASS |
| M1-invalid-id | ID 생략/NULL/0/음수/중복 거절과 저장 유지 | PASS |
| M1-strict-schema | 디코딩 후 중복 JSON 키 거부와 출력 DB 미생성 | PASS |
| M1-corruption | 잘린 payload 시작 거부와 손상 파일 보존 | PASS |
| M1-pipe | stdin EOF 전에 완성 문장 실행·출력 flush | PASS |
| M1-lease | 살아 있는 프로세스의 동일 실제 DBdir 별칭 차단, 다른 DBdir 허용·해제 후 재개 | PASS |
| M1-pending | 미해결 표식 시작 차단, 표식·데이터 보존 | PASS |
| M1-append-rollback | 실제 부분 append 뒤 실패, 원래 바이트/길이 복원·재시작 | PASS |
| M1-rollback-failure | 복원 실패 시 부분 파일/표식 보존, exit 6·재시작 차단 | PASS |
| M1-cleanup-committed | 반영 후 정리 실패에서도 성공/건수 유지, exit 5·뒤 문장 중단 | PASS |
| M1-output-failure | stdout 실패 exit 7, 정리 동반 시 exit 5, 반영 유지·후속 중단, 운영 바이너리 fault 환경 무시 | PASS |
| M1-read-ahead | 뒤 문장의 잘못된 UTF-8 이전에 첫 INSERT 반영, 분할 입력 | PASS |
| M1-argv-wire | help/version·인자 오류·18개 진단 필드·기본 표, 다섯 초기 출력 경로의 실제 stdout 실패 | PASS |

append/rollback/cleanup/DML stdout 실패는 별도 `minidb_fault.exe`에만 컴파일된 주입점을 쓴다. 부분 데이터·표식·바이트 길이와 독립 바이너리 해독/재시작을 함께 검사한다. 운영 `minidb.exe`가 `MINIDB_TEST_FAULT`를 무시하는 것도 실제 검사했다.

## 추가 독립 검사와 사용자용 시연

- `.cache/m1-reviewer/extra-results-2.json`: 36/36 PASS. 정수 양끝/범위초과·정규 십진 표현·빈 TEXT·NULL 등호·오류 코드/변경 없음, 엄격 스키마·전체 catalog 시작 검증, 저장 매직/버전/길이/태그/UTF-8 손상·스키마 순서 불일치·잔여 파일 차단을 확인했다. 검사 원문은 `.cache/m1-reviewer/extra_qa.py`다.
- `.cache/m1-reviewer/help-output-results-2.json`: `--help`, `init/run/migrate --help`, `--version` 다섯 실제 inherited read-only stdout handle 검사 PASS. exit 7·IO_ERROR/output/stdout 진단·미확인 DB 사실 null·원본 출력 파일 불변을 확인했다.
- Master 최종 데모 `.cache/m1-demo/session-kq5mvz5u/receipts.json`: init, INSERT SQL 파일, 별도 프로세스 SELECT JSONL, 별도 SELECT 기본 표 네 명령 모두 exit 0. `3.stdout`은 INT64 값/건수 문자열을 보존하고 `4.stdout`은 `1|"Kim"|25` 행을 표시했다.

## 기존 Given/When/Then 대응

원본은 SQL 67·스키마 41·저장 48·CLI 48, 합계 204개 명세다. 원본 문서의 작성 당시 미실행 표시는 그대로 보존하며 현재 실행 대응은 이 기록이 보완한다.

- **P1-SQL-001 전체 시나리오 확인:** M1-roundtrip이 정확한 INSERT SQL 파일·별도 프로세스 재시작 SELECT·INT64/TEXT 한 행·age/team NULL을 검증한다.
- 그 외 명세에는 리터럴/오류 중단·ID·스키마·저장·CLI의 검증된 부분이 있지만, 복합 사례 전체의 입력 변형·후속 동작을 일괄 실행하지 않았다. 예를 들어 복수 행 정수 경계 P1-SQL-005, 삭제 후 재사용 P1-SQL-016, 중복 SET까지 포함하는 P1-SQL-017은 부분 공통 동작이 있어도 전체 PASS가 아니다.
- 따라서 **나머지 203개 전체 사례는 미완료**로 남긴다. 부분 대응은 위 M1 시험의 구체 assertions와 추가 검사 원문 범위에서만 주장한다. 단순 명세 수·도구 probe·모의 하네스 검사를 기능 통과 수에 더하지 않는다.

## 결함·원인·수정

M1-QA-001은 root/subcommand help가 stdout 실패를 무시하고 version이 출력 진단 경로를 거치지 않는 문제였다. 일반 SQL 출력에는 있던 실패 처리가 초기 종료 분기에 빠져 있었다. `plain_output` 하나로 write 확인·IO_ERROR 진단·exit 7을 공통 처리하고 DB를 열지 않은 사실은 null로 유지했다. 기존 SQL 성공/반영·종료 우선순위는 바꾸지 않았다. 소스 변경은 `src/cli.c`, 추가 회귀는 `tests/milestone_cli.py`에 한정됐다.

M1 누적 반려는 Planner JSON findings 형식 1회와 위 QA 결함 1회로 2회다. Master는 두 번째 뒤 초기 종료 분기 누락을 재분석했고 두 항목 모두 해결됐다. bootstrap 하네스 유지보수의 별도 반려 2회와 합산하지 않는다.

## 미실행·한계·마감

- 전체 Phase 1 SQL·migrate·Phase 2 B+ 트리/자동 ID/100만 건 비교·Phase 3 API/Thread Pool은 미구현이다.
- 시작 시 행을 메모리에 적재하고 ID 중복을 이차 시간으로 검사한다. 현재 SELECT는 ID 조건도 선형 탐색이다. 대용량 성능을 주장하지 않는다.
- 정상 처리 가능한 실패의 문장 원자성을 다뤘다. 강제 종료/전원 차단 복구·완전 손상 탐지·모든 메모리 할당 실패·네이티브 콘솔 렌더링은 검증하지 않았다. inline 진단 경로 용량을 넘으면 잘라서 사실처럼 표시하지 않고 null로 둔다.
- Graphify vendor 추출 경고/일부 헤더 누락은 정적 도구의 한계다. 정적 그래프를 실행 증거로 쓰지 않는다.
- 문서·Wiki 대조 뒤 실제 21-finish가 exit 0으로 Graphify 갱신·Wiki 검사(원문/페이지 변경 없음)를 통과했다. 결과 문서화 뒤 재등록/최신 finish를 수행하며 영수증과 상세는 [DBMS-001 기록](records/DBMS-001.md)에 남긴다. 사용자 확인 전 run DONE/Goal 완료로 표시하지 않는다.

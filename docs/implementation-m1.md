# Phase 1 첫 마일스톤 구현 계획

2026-11-17 화요일(Asia/Seoul), 사용자가 승인한 **실제 구현** 범위다. 선택 출처는 Master가 전달한 현재 사용자 승인·재개 요약이며 개인 원장이나 대화 본문은 복제하지 않는다. 의미의 원본은 [SQL](sql-contract.md)·[스키마](schema-contract.md)·[저장](storage-contract.md)·[CLI](cli-contract.md) 계약이다.

## 범위와 순서

Windows x64 C23에서 init으로 새 DB를 준비하고 단일 행 INSERT·기본 SELECT를 파일 또는 명시적 stdin으로 실행한다. 기본 table/선택 JSONL, INT64·TEXT·NULL, 바이너리 저장과 프로세스 재시작 조회를 연결한다. 첫 오류 중단·이전 반영 보존, 엄격한 스키마/저장 손상 검사, 디렉터리 사용권·진행 표식, append 복원/복원 실패·반영 후 정리/출력 실패를 검증한다.

계약/GWT→실제 동작 Red→최소 Green→Refactor→누적 회귀를 따른다. 환경·import·빌드 실패는 기능 Red가 아니다. EOF나 전체 입력 선검증 때문에 완성한 첫 문장의 실행을 막지 않는다.

## 책임과 최소 API

| 경계 | 책임 |
|---|---|
| engine | DB·요청 수명, 실행 조정, 구조화 결과·진단·건강 상태 반환. |
| sql | 요청별 스캔·파싱·이름/타입 검증과 이번 SQL 실행. |
| catalog | 외부 JSON 전체 검증, 이름·타입·순서·ID 규칙과 시작 스냅샷. |
| storage | 바이너리 검증·초기 준비·사용권·표식·INSERT 반영/복원·정리. |
| cli | argv·입력 장치, table/JSONL 직렬화, stdout/stderr·flush·종료값. |

공개 API 원본은 [minidb.h](../include/minidb.h)다. md_open/md_close는 DB, md_request_open/md_request_next/md_request_close는 요청, md_operate는 관리 작업을 맡는다. MdInput read callback과 md_request_next pull 방식으로 문장별 진행한다. 바이트 read-ahead를 받더라도 후속 문장의 어휘·구문을 미리 검증해 현재 문장을 막지 않는다. CLI는 이벤트 출력·flush 확인 뒤 다음 이벤트를 요청한다.

MdEvent가 값·결과 메모리를 소유하고 DB/요청 종료 뒤에도 유효하다. md_event_destroy로 해제하며 0 초기화 이벤트도 해제할 수 있다. 같은 DB 호출은 직렬화하고 임시 파싱/실행 상태는 요청에 둔다. Phase 3 동시성 상세를 여기서 고정하지 않는다.

원본 변경 전에 직렬화·처리 건수·성공 결과 메모리를 준비한다. 고정 크기 MdDiagnostic은 반영 확정 뒤 할당 실패 때문에 성공 사실을 잃지 않게 한다. 이벤트는 성공 결과와 정리 진단을 함께 보존하고 committed/rollback/health/stop을 구분한다. stdout 실패는 이미 확정한 DML을 되돌릴 이유가 아니다.

## 실제 시험과 현재 증거

[c_checks.py](../scripts/c_checks.py)는 CMake 빌드·CTest의 새 JUnit을 연결한다. [milestone_cli.py](../tests/milestone_cli.py)는 별도 프로세스·고유 fixture로 출력·종료값·재시작 저장 상태를 검사한다. 실패 주입은 별도 시험 실행 파일을 사용하며 운영 실행 파일이 시험 환경 변수를 무시하는지도 확인한다.

| 등록 시험 | 확인할 동작 |
|---|---|
| M1-unit-event-lifetime | 이벤트 수명·해제 |
| M1-roundtrip | P1-SQL-001 파일 INSERT·재시작 SELECT·생략 NULL·표 출력 |
| M1-literals | BOM/주석·INT64 최솟값·TEXT/CRLF 보존 |
| M1-stop-on-error | 앞선 반영 유지·첫 오류 중단·마지막 세미콜론 |
| M1-invalid-id | ID 생략/NULL/0/음수/중복 거부·저장 불변 |
| M1-strict-schema | 디코딩 후 JSON 중복 키 거부·출력 생성 방지 |
| M1-corruption | 잘린 payload 시작 거부·파일 보존 |
| M1-pipe | EOF 전 실행·flush |
| M1-lease | 같은 실제 디렉터리 사용권·독립 DB 허용 |
| M1-pending | 미해결 표식 시작 차단·파일 보존 |
| M1-append-rollback | 부분 추가 실패 바이트/길이 복원·재시작 |
| M1-rollback-failure | 복원 실패 표식/부분 파일 보존·재시작 차단 |
| M1-cleanup-committed | 정리 실패에도 성공·건수 유지·뒤 SQL 중단 |
| M1-output-failure | stdout/정리 중복 실패 우선순위·반영 유지 |
| M1-read-ahead | 뒤의 잘못된 UTF-8보다 앞 문장 먼저 반영·분할 입력 |
| M1-argv-wire | help/version·인자 오류·진단 필드·기본 표 |

정식 run m1-native-20261117에서 최초 build 성공·실제 Red 1 PASS/15 FAIL을 확인한 뒤 구현했다. 최종 Implementer Green/회귀와 독립 Reviewer2 새 build/회귀가 16/16 PASS했고 추가 SQL/스키마/저장36개·실제 read-only stdout5개도 PASS했다. 단위 case 안에는 cmocka 함수 3개가 있다. 상세 원문 영수증·반려 수정·부분 GWT 대응은 [실제 결과](testing/m1-results.md)를 따른다. 최종 사용자 확인은 대기 중이다.

## 후속 증분과 완료 조건

후속 Phase 1은 [SQL 계약 9절](sql-contract.md)의 복수 행·NULL·조건식·VALUES→UPDATE/DELETE·계산·문장 원자성→정렬/페이지→JOIN·이름 해석→COUNT/GROUP BY/HAVING→UNION·전체 회귀 순서를 따른다. migrate는 기존 저장/CLI 계약에 따른 후속 관리 기능이다. Phase 2 B+ 트리·100만 건 비교와 Phase 3 API/Thread Pool은 연계 요구만 유지하고 해당 Phase에서 상세설계한다.

M1 완료에는 실제 Green·누적 회귀·독립 QA·검증된 지식 기록·마감 검사와 사용자 최종 확인이 필요하다. 등록 시험·도구 probe·모의 하네스 단위 시험을 DBMS 완료로 보고하지 않는다.
## 핵심 로직 설명과 실패 경계

- catalog는 직속 JSON 전부를 먼저 검사한다. 디코딩한 키 중복·미지 필드·잘못된 타입·식별자·파일명 불일치·ID 제약을 거절하고 선언 원철자/순서를 유지한다. 저장 헤더도 같은 스키마 구조로 검증한다.
- storage는 구조체 메모리를 통째로 쓰지 않고 고정 little-endian 프레임과 타입별 필드를 기록한다. 시작 때 모든 레코드 길이·UTF-8·ID를 읽고 검사한다. 현재 행 메모리 적재와 ID 중복 검사가 이차 시간이므로 대용량 성능 준비 완료를 뜻하지 않는다.
- INSERT는 검증·행 직렬화·메모리·성공 건수를 준비한 뒤 진행 표식을 기록/flush/close하고 EOF에만 추가한다. 전체 append와 FlushFileBuffers 성공이 반영 경계다. 그 전 실패는 원래 길이로 truncate/flush/길이 확인한다. 복원 불명은 파일/표식을 보존하고 중단한다.
- 반영 뒤 표식 정리 실패는 성공 이벤트·affected_rows를 보존한 채 cleanup 진단과 stop을 반환한다. CLI stdout 실패도 이미 반영한 DML을 되돌리지 않는다. 종료 우선순위 6>5>7>4>3>2를 유지한다.
- sql은 입력 callback에서 받아 둔 바이트를 문장 단위로 소비한다. 뒤 문장의 잘못된 UTF-8 때문에 앞 문장 실행을 막지 않는다. 작은따옴표·주석 안 세미콜론은 경계가 아니며 CRLF/UTF-8 경계는 요청 상태와 1바이트 callback 시험으로 확인한다.
- 이벤트가 결과를 소유하므로 DB/요청 해제 뒤에도 값이 유효하다. 반영 진단은 고정 크기로 보존한다. inline 경로 용량 초과는 잘라 사실처럼 보이지 않고 null로 보고한다.
- 별도 fault 실행 파일에만 실패 주입을 컴파일한다. 일반 바이너리는 시험 환경변수를 무시한다. 단위·CLI·실제 프로세스/저장 검사 범위를 구분한다.

구현·QA 완료는 M1 범위이며 전체 Phase 1·migrate·후속 Phase나 전원 차단 복구를 완료했다는 뜻이 아니다. Goal 유지 지시와 도구 usageLimited 상태도 구분한다.

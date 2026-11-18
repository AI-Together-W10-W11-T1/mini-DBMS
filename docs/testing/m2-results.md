# M2 실제 구현·검증 결과

2026-11-17 화요일(Asia/Seoul). 요구사항 `DBMS-002`, 정식 run `m2-native-20261117`의 **구현·독립 QA 통과, 사용자 완료 승인·DONE** 기록이다. 사용자 선택 1-A·2-A·3-B와 후속 진행 지시를 Master가 구현 승인으로 전달했다. M1은 사용자 승인·DONE·Goal complete 상태이며 M2의 새 Goal은 생성하지 않았다.

## 구현한 동작과 불변식

- 복수 행 INSERT는 모든 행의 컬럼 수·타입·정수 범위·ID와 문장 내부/기존 ID 중복을 검증한다. 전체 프레임·메모리·성공 건수를 준비한 뒤 한 번의 문장 단위 append를 수행한다.
- 전체 쓰기와 `FlushFileBuffers` 성공이 반영 경계다. 반영 전 실패는 원래 길이로 복원·flush·길이 확인하고, 복원 실패는 파일·표식을 보존해 재시작을 차단한다. 반영 뒤 정리·stdout 오류에서도 전체 삽입 건수를 보존하고 후속 문장을 중단한다.
- WHERE는 6종 비교·NULL 검사·AND/OR·괄호를 지원한다. 모든 조건 가지의 이름·타입을 행 평가 전에 검사하며, 후위식과 명시적 스택으로 괄호/우선순위를 처리한다. NULL의 3값 논리에서 TRUE인 행만 반환한다.
- 단일 테이블 수식 참조와 입력/출력 AS를 지원한다. 입력 별칭은 원래 테이블 수식자를 가리고 출력 별칭은 WHERE 입력 범위를 바꾸지 않는다. 최종 출력 이름은 ASCII 대소문자를 무시해 중복 검사한다.
- 독립 VALUES는 위치별 타입을 전체 행에서 추론하고 전부 NULL인 열에 기존 `MD_NULL` 결과 메타데이터를 사용한다. 후반 오류에서 부분 결과를 공개하지 않는다. 이벤트는 DB·요청 종료 뒤에도 결과를 소유한다.

다섯 책임·공개 API·CLI 규약·기존 바이너리 레코드 포맷을 유지했다. 소스 변경은 `src/sql.c`, `src/storage.c`, `src/internal.h`, `tests/m2_cli.py`, `tests/unit_milestone.c`, `CMakeLists.txt`, `scripts/c_checks.py`다. 최종 기술 소스 digest는 `d53bf142aa3ae5b81bf2bb654c526c78bb3d9f8d563ad58b20694fc0f0a62474`다.

## 실제 Red·Green·독립 검증

아래 디렉터리는 `.cache/agent-harness/m2-native-20261117/commands/` 기준이다. 각각의 `receipt.json`과 stdout/stderr 로그, 시험 명령의 `test-report.json`이 증거다. 캐시는 배포·추적 소스가 아니므로 삭제 뒤에는 고정 소스로 재실행해야 한다.

| 검사 | 실제 결과 | 증거 |
|---|---|---|
| 최초 build | configure·compile·link exit 0 | `1-build` |
| 기능 Red | CTest exit 8, report errors 빈 배열. 새 M2 14개와 확장 cmocka case가 런타임 assertion 실패, 기존 CLI 15개 PASS | `2-red` |
| 첫 Green | 28/30 PASS. 새 시험 fixture 두 곳을 교정 | `3-build`, `4-green` |
| 교정 뒤 Green·회귀 | 30/30 PASS | `5-build`, `6-green`, `7-regression` |
| 문법 오류 분류 보완 뒤 회귀 | 30/30 PASS | `8-build`, `9-green`, `10-regression` |
| 최종 Implementer 검사 | build·Green·regression 각각 exit 0, 30/30 PASS | `11-build`, `12-green`, `13-regression` |
| 독립 Reviewer_QA | 새 build·regression 각각 exit 0, 30/30 PASS, report errors 빈 배열 | `14-build`, `15-regression` |

최초 Red 전에 생산 C 소스가 이전 snapshot과 동일함을 Master가 확인하고 정상 `confirm-red` 명령으로 기록했다. 단위 실행 파일의 원래 세 함수와 기존 CLI 15개는 통과했고, 새 M2 함수가 복수 INSERT assertion에서 실패했다. 빌드·환경 실패를 기능 Red로 세지 않았다.

첫 Green의 두 시험 오류는 `ABSENT`가 예약어인 점과 공용 Fixture.init이 테이블 이름을 소문자 users로 고정 기대한 점이었다. 미존재 테이블 입력은 `missing_table`로, 원철자 Users의 초기 준비는 실제 선언 이름의 결과를 직접 확인하도록 교정했다. 기존 M1 시험과 의도한 오류/원철자 기대를 약화하지 않았다. 잘못 반복된 IS/WHERE·괄호·조건 끝 누락은 문법 오류로, NOT/COUNT/집계 같은 알려진 미지원 기능은 기존 코드로 분류하도록 구현과 회귀를 보완했다.

독립 결과는 `.cache/m2-master/reviewer-result.json`과 정식 `results/reviewer_qa-1.json`에 review APPROVED·qa PASS로 분리돼 있다. M2 누적 품질 반려는 0회이며 M1의 과거 반려와 합산하지 않는다.

## 등록 시험과 확인 범위

CTest는 M1 16개와 M2 14개를 등록한다. `M1-unit-event-lifetime` 한 case 안에는 기존 세 함수와 새 `m2_owned_values_and_aliases`를 합친 cmocka 함수 네 개가 있다.

| 등록 시험 | 확인한 동작 | 독립 결과 |
|---|---|---|
| M2-INSERT-01 | SQL 파일 복수 행·건수·생략 NULL·별도 프로세스 조회와 독립 바이너리 해독 | PASS |
| M2-INSERT-02 | 문장 내부/후반 기존 ID 충돌과 파일 바이트 보존 | PASS |
| M2-INSERT-03 | 후반 개수·타입·범위·ID 오류 전체 미반영, INT64 양끝 저장 | PASS |
| M2-INSERT-04 | 부분 append 원래 바이트 복원·재시작, 복원 실패 표식·부분 파일·exit 6 | PASS |
| M2-INSERT-05 | 반영 후 cleanup/stdout/동반 실패의 전체 건수·종료 우선순위·후속 중단, 운영 바이너리 주입 무시 | PASS |
| M2-WHERE-01 | 6종 INT64 비교, 리터럴/컬럼 조합, NULL, 대소문자·빈 문자열·공백·한글 TEXT 바이트 비교 | PASS |
| M2-WHERE-02 | 대칭 AND/OR 3값 조합의 반환 행, IS NULL/IS NOT NULL, 괄호·우선순위 | PASS |
| M2-WHERE-03 | 빈/비어 있지 않은 테이블의 정적 이름·타입 오류, 잘못된 구문·미지원 기능 분류 | PASS |
| M2-NAME-01 | 테이블/별칭 수식 참조, 원철자·스키마 순서, 출력 AS와 별칭의 별표 조회 | PASS |
| M2-NAME-02 | 별칭의 원래 이름 가림, 출력 AS의 WHERE 범위, 최종 이름 중복·서로 다른 AS | PASS |
| M2-VALUES-01 | 위치별 INT64/TEXT 결과·기본 이름, 저장 파일·ID 불변 | PASS |
| M2-VALUES-02 | NULL 위치별 추론과 전부 NULL 열의 table/JSONL 메타데이터 | PASS |
| M2-VALUES-03 | 후반 개수·타입·범위·문법 오류의 부분 결과 미공개 | PASS |
| M2-REGRESSION-01 | 기존 프레임·재시작, 혼합 SQL 파일, 앞선 반영 보존·후속 중단, stdin EOF 전 VALUES 출력 | PASS |

기존 M1 16개도 모두 PASS다. 실패 주입은 별도 `minidb_fault.exe`에만 컴파일되며 실제 파일 길이·바이트·표식과 재시작 결과를 검사한다. 모든 메모리 할당 실패나 모든 장치 오류 지점을 검증한 것으로 확대하지 않는다.

## 추가 검사와 M1 파일 호환

- 독립 `.cache/m2-reviewer/extra_qa.py`와 `extra-results.json`은 104/104 PASS다. 4096중첩 괄호, 512개 AND/OR 연결, seed 20261117의 혼합 조건식 80개, 정수 극값·별칭·NULL VALUES·문법 오류를 실제 실행했다. 조건별 반환 행은 Python의 3값 기대 평가와 대조했다. 결과 필터에서 UNKNOWN과 FALSE가 모두 제외되는 한계는 소스의 내부 진리표 검토로 함께 확인했다.
- Master의 `.cache/m2_demo.py`, `.cache/m2-demo/receipt.json`은 실제 캐시 M1 실행 파일로 init·단일 INSERT한 파일을 M2 실행 파일로 열어 복수 INSERT·별칭 WHERE·VALUES를 수행한 증거다. 삽입 건수 2, Kim/Lee 두 행, INT64로 추론된 NULL/1 결과를 확인했다. 이 실제 M1 작성 시연과 독립 등록 회귀의 기존 프레임 확인을 구분한다.

## 기존 Given/When/Then 대응

원본 SQL 67·스키마 41·저장 48·CLI 48, 합계 204개 인수 명세를 한꺼번에 완료 처리하지 않는다. 다음은 실제 시험 원문의 의미 대응이다.

| 기존 명세 | M2 실행 근거 |
|---|---|
| P1-SQL-005 | M2-INSERT-03의 정확한 양끝 INT64 두 행·건수·재조회 |
| P1-SQL-013/014 | M2-INSERT-01/02의 복수 행·생략 NULL·내부 및 기존 ID 충돌. 독립 fixture ID는 5/6을 사용 |
| P1-SQL-029/030 | M2-WHERE-01/02의 NULL 비교/검사와 양쪽 컬럼·리터럴 조건 |
| P1-SQL-031/032 | M2-WHERE-03/01의 빈 테이블 정적 오류, TEXT 대소문자·빈 문자열·공백 |
| P1-SQL-052/053/054 | M2-VALUES-01/02/03의 독립 결과·파일 불변·위치 타입 추론·후반 오류 |

P1-SQL-017/027은 UPDATE 부분, 028/033/034는 정렬 부분, 043은 JOIN 부분을 포함하므로 이번 공통 동작의 실행만으로 전체 사례 PASS를 부여하지 않는다. M1 시점의 전체 P1-SQL-001 검증은 [M1 결과](m1-results.md)에 보존한다. 다른 사례도 실제 assertions를 대조한 범위에서만 보고한다.

## 미완료·마감·다음 행동

UPDATE/DELETE·계산, 정렬/페이지, JOIN, 집계, UNION, migrate와 Phase 2/3는 후속이다. 시작 시 행 적재·이차 시간 ID 검사·선형 SELECT는 그대로이며 100만 건 성능, 전원 차단/강제 종료 복구, 모든 손상·할당 실패·실제 콘솔 렌더링은 검증하지 않았다.

이 문서·Wiki 원문 대조 뒤 실제 정식 `16-finish`와 최신 `17-finish`가 exit 0으로 Graphify 갱신·Wiki 변경 원문 없음/페이지 변경 없음을 확인했다. vendor yyjson 두 파일의 부분 추출과 네 파일 no-symbol 경고는 정적 도구 한계로 남는다. 영수증은 `commands/*-finish/receipt.json`과 [DBMS-002 기록](records/DBMS-002.md)을 따른다. Master가 전체 결과를 보고했고 사용자가 M2 완료를 명시 승인했다. 정상 `finish --user-ref` 명령으로 하네스 DONE을 확인한 뒤 이 완료 상태 문서를 별도 유지보수로 갱신했다. 다음 행동은 후속 UPDATE/DELETE·계산 증분 계획 협의이며 새 구현 승인은 아니다.

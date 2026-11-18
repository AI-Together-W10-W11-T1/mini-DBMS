# M5 JOIN 구현·검증 결과 — DBMS-005

2026-11-18 수요일(Asia/Seoul). 정식 run `m5-native-20261118`, 정책 `slim-v1`. **구현·독립 QA와 사용자 완료 승인·하네스 DONE을 확인**했다. 새 Goal은 없다.

## 승인 범위와 구현

Master가 전달한 현재 사용자 A 선택은 두 입력 INNER JOIN 전체·명시적 다른 AS self-join·기존 WHERE·M4 정렬/페이지 통합, 중첩 루프, 독립 QA·문서화 뒤 최종 확인 한 번이다. 중간 확인을 나누는 B 대신 전체 증분 A를 선택했다. 출처는 `.cache/m5/contract.json`, 정식 planner/knowledge packet의 approval_summary와 `.cache/m5/planner-result.json`이다. 별도 계획·중복 요구사항 record를 만들지 않는다.

의미의 원본은 [SQL 계약](../sql-contract.md)과 [SQL 인수 명세](phase1-sql-acceptance.md)다. `src/sql.c`에서 요청별 최대 두 입력 범위를 결합하고 ON의 같은 타입 양 입력 컬럼 등호 하나를 검증한다. NULL 키는 연결하지 않고 중복 키의 모든 조합을 보존한다. TEXT는 UTF-8 바이트로 비교한다. self-join은 서로 다른 명시적 AS가 필요하고, 별칭은 원래 테이블 이름을 숨긴다. 무수식 중복 컬럼·입력 이름 충돌·중복 출력 이름은 오류다. 기본 출력 이름은 유효 입력 이름과 스키마 컬럼 원철자를 조합하며 별표는 왼쪽 다음 오른쪽 스키마 순서다. 출력 AS 뒤 ORDER BY는 최종 출력 이름만 참조한다.

중첩 루프는 Cartesian product 전체 배열을 만들지 않고 ON→WHERE를 통과한 출력만 누적한다. 한 행 폭 scratch의 TEXT는 카탈로그에서 빌리고 해제하지 않으며 결과에는 깊은 복사한다. 결과 행 증가·셀 수·바이트 수와 입력 폭은 연산 전에 검사한다. 부분 복사 실패도 초기화한 행 전체를 이벤트 정리가 회수한다. 빈 입력·WHERE 0행·FETCH 0도 정적 이름/타입/출력/끝절 검증을 생략하지 않는다. 공개 API·저장 포맷·의존성은 변경하지 않았다.

변경 기술 파일은 `src/sql.c`, [M5 시험](../../tests/m5_cli.py), `tests/unit_milestone.c`, `CMakeLists.txt`, `scripts/c_checks.py` 다섯 개다. 최종 기술 digest는 `b89e27fd1787d5397ae6d5a0eadd31644cf898d37df5a808e8825f476cbebe26`이며 작성자 최종·독립 최종 영수증이 일치한다.

## Given/When/Then과 인수 대응

| 등록 검사 | Given / When / Then 요약 |
|---|---|
| M5-JOIN-01 | users/orders / 별칭 유무·정역 ON INT64 JOIN / (1,10),(1,11),(2,12), NULL 키 제외. SQL040 대응 |
| M5-JOIN-02 | 같은 키 왼쪽2·오른쪽3 / JOIN / 6조합과 중복 보존. SQL041 대응 |
| M5-JOIN-03 | 빈문자열·공백·대소문자·한글·CRLF·NULL / TEXT JOIN 및 INT64 최솟값 / 바이트 동등성·NULL 제외 |
| M5-JOIN-04 | users AS a/b / team JOIN과 다른 id WHERE / (1,2),(2,1), 명시 AS 없는 self-join 거절. SQL042 대응 |
| M5-NAME-01 | 중복/고유 컬럼·별칭 / 출력·WHERE·ON 결합 / 모호함·없는 이름·입력 중복 오류. SQL043 대응 |
| M5-NAME-02 | 원철자 스키마·AS / 별표·직접 출력·출력 AS / 좌우 헤더·수식명·중복 거절·최종 ORDER 이름. SQL044 대응 |
| M5-TAIL-01 | 다중 매치와 NULL / 양 입력 WHERE·다중 정렬·OFFSET/FETCH / 단계 순서와 최종 한 행 |
| M5-EMPTY-01 | 한쪽/양쪽 빈 입력·미일치 / 정상·오류 조회와 FETCH0 / 타입 있는 빈 헤더·정적 오류 유지 |
| M5-ERROR-01 | 두 입력 / 한쪽 ON·타입·다중 조건·범위 밖 JOIN·필수 토큰 누락 / 계약 오류·부분결과 없음. SQL045 대응 |
| M5-RESOURCE-01 | 실패 주입 빌드·두 저장파일 / 선택한 폭·행·부분 복사 실패 / RESOURCE_ERROR·두 파일 불변·운영 빌드 주입 무시 |
| M5-OWNERSHIP-01 | 실제 TEXT/NULL 결과 / 요청·DB 종료·이벤트 해제·실제 fault unit 실행 / 소유권 유지·크기 경계·부분 실패 뒤 재시도 |
| M5-CLI-01 | 파일/stdin·table/JSONL·앞성공/오류/후속변경 / 실행 / 출력 일치·진단·앞성공 보존·후속 중단 |
| M5-REGRESSION-01 | 두 저장파일·기존 기능 / JOIN 전후 VALUES·UPDATE·DELETE·별도 프로세스 재시작 / 파일 불변과 기존 기능 유지 |

SQL040~045는 위 실행 의미의 대응이다. SQL044 시험은 label 컬럼이 추가된 orders fixture를 쓰며 원본의 좌우 스키마 확장 규칙을 검증한다. 위 표와 기존63개 회귀는 SQL67·스키마41·저장48·CLI48의 전체204개 통과 선언이 아니다. 집계·UNION·migrate는 미구현이다.

## 실제 Red·회귀·독립 QA

명령은 `python scripts/agent_harness.py --run m5-native-20261118 --summary run-check <red|targeted|regression>`이며 아래 경로는 `.cache/agent-harness/m5-native-20261118/commands/` 기준 receipt·test-report·새 JUnit·로그다. 모두 실제 configure/build/CTest를 실행했다. 캐시는 배포물이 아니며 삭제 후 고정 소스로 재실행해야 한다.

| 명령 디렉터리 | 실제 결과 |
|---|---|
| 1-red~4-red | 각 build0·CTest exit8, 선택4개 assertion 실패·errors=[]; 최종 Red4를 Master 확인 후 구현 |
| 5-targeted | 13/14 PASS·exit8. JOIN-02 fixture의 네 번째 매치 행을 미일치로 바로잡음 |
| 6-targeted | 강화한 선택3/3 PASS·exit0 |
| 7-regression | 작성자76/76 PASS·exit0 |
| 8-regression | 독립 VERIFY cold76/76 PASS·exit0. 이후 별도 음성 재현에서 R1 발견 |
| 9-red | 수정 전 M5-ERROR-01 0/1·exit8·build0·errors=[]; 기대 SYNTAX_ERROR/실제 INVALID_JOIN |
| 10-regression | R1 수정 뒤 작성자76/76 PASS·exit0·errors=[] |
| 11-regression | 독립 최종76/76 PASS·exit0·errors=[], review APPROVED·qa PASS |

최초 확인 Red는 기존 `src/sql.c` SHA `202b23a480c143aa1862140e5ee57d750ce49973b1450616922ec397194b3f81` 상태에서 실제 JOIN·TEXT 소유권·자원 기대가 실패한 것이다. fixture 환경 변수/스냅샷/인용 수정 전 시도도 보존했다. 합성 폭 경계는 baseline 할당 방어로 이미 통과했으므로 overflow 전용 실패 Red였다고 주장하지 않는다. 환경/import 실패는 기능 Red에 포함하지 않는다.

누적 품질 반려는 **1회**다. 독립 첫 검토의 `R1`(Master 추적 `M5-QA-001`)은 `ON u.id o.user_id`의 필수 `=` 누락을 INVALID_JOIN으로 오분류한 결함이다. 비교 토큰 없음과 존재하는 비등호를 함께 분류한 것이 원인이며 CMP_NONE을 먼저 문법 오류로 처리했다. 수정 전 실제 Red9 후, 등호 누락·왼쪽 뒤 세미콜론·EOF·오른쪽 누락 네 변형을 기존 M5-ERROR-01에 추가했다. `>`의 INVALID_JOIN 기대는 유지했다. 독립 최종 경계 검사가 해결을 확인했다. 초기 targeted fixture 수정과 독립 script의 예약어 ABSENT→no_such_col 수정은 제품 결함이나 별도 반려가 아니다.

독립 추가 검사는 시드20261118의 **oracle32/32 PASS**(8개 INT64/TEXT 생성 데이터·각4모드, Python 중첩 루프·NULL·중복·정역 ON·WHERE·정렬/페이지)와 **경계19/19 PASS**(음성18·양성1)다. `.cache/m5/reviewer/oracle-v2.py`, `oracle-report-v2.json`, `negative-v2.py`, `negative-report-v2.json`에 SQL·기대·실제가 남아 있다. 첫 검토는 R1 발견 뒤 oracle 실행 전 중단했다. 추가51개를 CTest76개에 더하지 않는다.

CTest76개는 기존63개+M5 13개다. 운영 cmocka8개는 기존6+M5 2이며 fault10개에는 운영8개 반복과 fault 전용2개가 포함된다. M5-OWNERSHIP wrapper는 이를 실제 호출하며 독립적인 새 unit 수로 합산하지 않는다.

## 관측·한계·다음 행동

영수증 호출 수는 Red5회(초기4+결함1), targeted2회, 전체 회귀4회(작성자2·독립2)다. 최종 작성자 report 단계 합은59.86s(configure4.98·compile3.24·CTest51.63), 독립 최종 receipt는54.0s(report 단계 합53.62s)다. 독립 최초 cold는111.313s였다. 역할별 캐시·cold/warm·측정 경계가 달라 시간 비교로 절감률이나 인과효과를 주장하지 않는다.

재작업 관측153.65s는 attempt2 시험 준비02:20:46Z~최종 freeze02:23:19Z의 부분 구간이며 Master Red 확인 대기·전체 회귀를 포함한다. 직전 packet/검토 읽기와 전체 검토 wall-clock은 포함하지 않는다. 실제 provider token은 제공되지 않았으며 비용 절감·TDD 효과는 미측정이다. 출처는 작성자/독립 `*-result-v2.json`과 해당 영수증이다.

중첩 루프 O(L×R)이며 일치 출력 전체를 메모리에 보관한 뒤 정렬·페이지를 적용한다. 작은 결정적 데이터·선택한 실제 실패 주입·동일 크기 함수 경계를 검증했으며 거대 실제 데이터·100만 건 성능·모든 할당 실패·heap leak instrumentation·전원 차단/강제 종료 복구는 미검증이다. 기존 저장/CLI 한계는 [M4 결과](m4-results.md)와 원래 계약에 보존한다. Knowledge 단계는 제품 검사를 재실행하지 않았다.

2026-11-18 수요일 사용자가 M5 완료를 명시 승인했고 Master가 문서 변경 전에 정상 `finish --user-ref`로 DONE을 확인했다. 출처는 Master의 현재 사용자 지시 요약과 정식 state.json의 phase·completion_user_ref다. 이번 갱신은 완료 후 문서 유지보수이며 제품 검사는 재실행하지 않았다. 다음 증분은 COUNT·GROUP BY·HAVING의 계획 협의이며 구현 승인이 아니다. Phase2 B+트리·Phase3 API/Thread Pool 상세 설계도 해당 단계에서 정한다. 커밋·게시·새 과금은 하지 않았다.

## 문서 마감

승인7개 문서만 갱신하고 기존 Wiki61개 출처를 보존해 변경 원문·M5 결과/시험을 대조한 뒤 `wiki-review --reviewed`, 정상 `run-check finish`를 수행한다. 실제 마감 관측은 아래에 기록한다.

첫 `12-finish`는 exit0·7.625s·기술 digest 불변, Wiki missing_page=false·changed_sources=[]·page_changed=false였다. 실제 Graphify task-end 색인1회는 code34·노드1276·간선4982, LLM 의미 추출 없이 수행했다. 정적 색인은 실행 검증을 대체하지 않는다. 승인 문서 UTF-8·로컬 링크137개·기술5개 SHA 불변을 확인했고 Wiki 기존61개 목록에 결과/시험2개를 더해63개로 등록했다. 기존 목록 중 이번 변경 밖53개 hash는 그대로 보존했다. 이 관측 추가 뒤 Wiki 재등록·최종 `13-finish`를 수행하며 최종 영수증과 반환 JSON에 결과를 남긴다. 제품 검사는 재실행하지 않는다.

완료 상태 반영 뒤 Wiki를 원문과 재대조해 등록하고 직접 `python scripts/dev_tools.py finish`로 마감한다. DONE run은 재개하지 않는다. 최종 영수증·문서 hash·기술 hash 불변·추가 Graph 호출 여부는 `.cache/m5/completion-result.json`에 기록한다.
